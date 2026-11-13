#include "screens_test.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// The boxes: a machine's menu and its questions, keys and text, the wall as a file, a KVM added
// and changed and its certificates, signing in and the account, and the settings.
class TestBoxScreens : public ScreensTest {
    Q_OBJECT

private slots:
    // After each test, when its windows are gone: what they were handed goes too.
    void cleanup() { forgetWhatTheWindowsWereHanded(); }

    void aMachinesMenuShowsWhatItIsGivenAndSaysWhatWasChosen()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(1280, 1000);
        QVERIFY(bringUp(window));
        QObject* menu = window->findChild<QObject*>(u"machineMenu"_s);
        QVERIFY(menu != nullptr);
        QVERIFY(!menu->property("opened").toBool());
        const QSignalSpy chosen(&actions, &qkvm::ui::ActionsView::chosen);
        const QSignalSpy closeAsked(&actions, &qkvm::ui::ActionsView::closeAsked);

        // The program opens it, and it shows what it was given: the machine, how it is, and under
        // each heading the things to do.
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(menu->property("opened").toBool());
        for (const QString& text : {u"NAS"_s,
                                    u"192.168.1.22 · KVM-A · 4.82"_s,
                                    u"The computer is off, asleep or not plugged into the KVM."_s,
                                    u"Enlarge"_s,
                                    u"Computer on screen"_s,
                                    u"Build box"_s,
                                    u"port 2"_s,
                                    u"on screen"_s,
                                    u"Keyboard"_s,
                                    u"Wake screen"_s,
                                    u"Shift"_s,
                                    u"Ctrl+Alt+Del"_s,
                                    u"Close window"_s,
                                    u"Alt+F4"_s,
                                    u"Your actions"_s,
                                    u"Lock it"_s,
                                    u"Win+L"_s,
                                    u"Power"_s,
                                    u"Keep awake"_s,
                                    u"Press power button"_s,
                                    u"Hold power button"_s,
                                    u"Press reset button"_s,
                                    u"This KVM"_s,
                                    u"Reboot KVM"_s}) {
            QVERIFY2(saying(window->contentItem(), text) != nullptr, qPrintable(text));
        }
        // All of it inside the window.
        const QQuickItem* last = theOne(window, u"row:reboot"_s);
        QVERIFY(last != nullptr);
        QVERIFY(last->mapToScene(QPointF(0, last->height())).y() < window->height());
        keepPictureOf(window, u"machine-menu"_s);

        // A row is chosen: the program is told which.
        const QQuickItem* hold = theOne(window, u"row:power:atx_power_long"_s);
        QVERIFY(hold != nullptr);
        clickOn(window, hold);
        QTRY_COMPARE(chosen.count(), 1);
        QCOMPARE(chosen[0][0].toString(), u"power:atx_power_long"_s);
        // The computer that is on the screen already cannot be chosen.
        const QQuickItem* onScreen = theOne(window, u"row:port:0"_s);
        QVERIFY(onScreen != nullptr);
        QVERIFY(!onScreen->isEnabled());
        clickOn(window, onScreen);
        // While something it sent is being answered, the menu holds still.
        actions.setBusy(true);
        clickOn(window, hold);
        QTest::qWait(150);
        QCOMPARE(chosen.count(), 1);
        actions.setBusy(false);
        // With the keyboard: Return on a row chooses it.
        QVERIFY(tabTo(window->contentItem(), u"row:keys:Enter"_s));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(chosen.count(), 2);
        QCOMPARE(chosen[1][0].toString(), u"keys:Enter"_s);

        // The menu follows the machine: a row that is no longer offered is gone.
        QVariantMap other = menuOfAKvm();
        other[u"sections"_s] = other.value(u"sections"_s).toList().mid(0, 1);
        actions.showMenu(other);
        QTRY_VERIFY(theOne(window, u"row:reboot"_s) == nullptr);
        QVERIFY(theOne(window, u"row:enlarge"_s) != nullptr);
        QVERIFY(menu->property("opened").toBool());
        // The battery of its Fingerbot, where that is low, is warned of at the top; and nothing is there without.
        const QQuickItem* battery = theOne(window, u"menuBattery"_s);
        QVERIFY(battery != nullptr);
        QVERIFY(!battery->isVisible());
        other[u"battery"_s] = u"The battery of its Fingerbot is low (9 %). A press may fail: charge it."_s;
        actions.showMenu(other);
        QTRY_VERIFY(saying(window->contentItem(),
                           u"The battery of its Fingerbot is low (9 %). A press may fail: charge it."_s) != nullptr);
        QVERIFY(battery->isVisible());

        // Closed by hand, the program is told; and it is the program that closes it.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(closeAsked.count(), 1);
        actions.showMenu({});
        QTRY_VERIFY(!menu->property("opened").toBool());
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(menu->property("opened").toBool());
        actions.showMenu({});
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(closeAsked.count(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aMenuLongerThanTheWindowScrollsAndFollowsTheKeyboard()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        // As low as a window gets.
        window->resize(1000, 400);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->height(), 400);
        QObject* menu = window->findChild<QObject*>(u"machineMenu"_s);
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(menu->property("opened").toBool());
        // The box itself is inside the window, and says that there is more of it.
        const QQuickItem* mark = theOne(window, u"menuScrollMark"_s);
        QVERIFY(mark != nullptr);
        QTRY_VERIFY(mark->isVisible());
        const QQuickItem* first = theOne(window, u"row:enlarge"_s);
        const QQuickItem* last = theOne(window, u"row:reboot"_s);
        QVERIFY(first != nullptr);
        QVERIFY(last != nullptr);
        const auto inSight = [&](const QQuickItem* row) {
            const qreal top = row->mapToScene(QPointF(0, 0)).y();
            const qreal ends = mark->parentItem()->mapToScene(QPointF(0, mark->parentItem()->height())).y();
            return top >= mark->parentItem()->mapToScene(QPointF(0, 0)).y() && top + row->height() <= ends;
        };
        QVERIFY(mark->parentItem()->mapToScene(QPointF(0, mark->parentItem()->height())).y() <= window->height());
        QVERIFY(inSight(first));
        QVERIFY(!inSight(last));
        keepPictureOf(window, u"machine-menu-low"_s);

        // The keyboard comes to the last row: it is brought into sight, and the mark has moved.
        const qreal markWas = mark->y();
        QVERIFY(tabTo(window->contentItem(), u"row:reboot"_s));
        QTRY_VERIFY(inSight(last));
        QVERIFY(!inSight(first));
        QVERIFY(mark->y() > markWas);
        keepPictureOf(window, u"machine-menu-low-end"_s);
        // And back to the first.
        QVERIFY(tabTo(window->contentItem(), u"row:enlarge"_s));
        QTRY_VERIFY(inSight(first));

        // Opened anew, it starts at its top.
        QVERIFY(tabTo(window->contentItem(), u"row:reboot"_s));
        QTRY_VERIFY(inSight(last));
        actions.showMenu({});
        QTRY_VERIFY(!menu->property("opened").toBool());
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(theOne(window, u"row:enlarge"_s) != nullptr && inSight(theOne(window, u"row:enlarge"_s)));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatCannotBeTakenBackIsAskedInItsOwnBox()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* question = window->findChild<QObject*>(u"question"_s);
        QVERIFY(question != nullptr);
        QVERIFY(!question->property("opened").toBool());
        const QSignalSpy answered(&actions, &qkvm::ui::ActionsView::answered);

        // Over the menu it came from.
        actions.showMenu(menuOfAKvm());
        actions.setBusy(true);
        actions.showQuestion(aQuestion());
        QTRY_VERIFY(question->property("opened").toBool());
        QVERIFY(saying(window->contentItem(), u"Hold power button on NAS?"_s) != nullptr);
        QVERIFY(saying(window->contentItem(), u"A long press cuts the power. Unsaved work is lost."_s) != nullptr);
        const QQuickItem* yes = theOne(window, u"questionYes"_s);
        const QQuickItem* no = theOne(window, u"questionNo"_s);
        QVERIFY(yes != nullptr);
        QVERIFY(no != nullptr);
        // The yes is called what it does, and the no is where the keyboard starts.
        QVERIFY(says(yes, u"Hold power button"_s));
        QVERIFY(says(no, u"Cancel"_s));
        QTRY_VERIFY(no->hasActiveFocus());
        keepPictureOf(window, u"question"_s);

        clickOn(window, yes);
        QTRY_COMPARE(answered.count(), 1);
        QCOMPARE(answered[0][0].toBool(), true);
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());
        // Taken away by the program, it has not been answered a second time.
        QTest::qWait(100);
        QCOMPARE(answered.count(), 1);

        // Esc is a no.
        actions.showQuestion(aQuestion());
        QTRY_VERIFY(question->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(answered.count(), 2);
        QCOMPARE(answered[1][0].toBool(), false);
        actions.showQuestion({});
        QTest::qWait(100);
        QCOMPARE(answered.count(), 2);
        // The menu behind it was not closed by that.
        QVERIFY(window->findChild<QObject*>(u"machineMenu"_s)->property("opened").toBool());

        // And so is Cancel; with the keyboard, which is there already.
        actions.showQuestion(aQuestion());
        QTRY_VERIFY(question->property("opened").toBool());
        QTRY_VERIFY(theOne(window, u"questionNo"_s) != nullptr && theOne(window, u"questionNo"_s)->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(answered.count(), 3);
        QCOMPARE(answered[2][0].toBool(), false);
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(answered.count(), 3);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxForKeysSaysWhatWasChosen()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"sendKeys"_s);
        QVERIFY(box != nullptr);
        QVERIFY(!box->property("opened").toBool());
        const QSignalSpy keysAsked(&actions, &qkvm::ui::ActionsView::keysAsked);
        const QSignalSpy closeAsked(&actions, &qkvm::ui::ActionsView::formCloseAsked);
        const QVariantMap forKeys{{u"kind"_s, u"keys"_s}, {u"machine"_s, u"NAS"_s}, {u"problem"_s, QString()}};

        actions.showForm(forKeys);
        QTRY_VERIFY(box->property("opened").toBool());
        // The page's four keys to hold, the list of the others, and nothing chosen yet.
        for (const QString& text :
             {u"Send keys"_s, u"Pressed together on the connected computer, then released."_s, u"Ctrl"_s, u"Alt"_s,
              u"Shift"_s, u"Win"_s, u"No other key"_s, u"Nothing chosen"_s, u"Cancel"_s, u"Continue"_s}) {
            QVERIFY2(saying(window->contentItem(), text) != nullptr, qPrintable(text));
        }
        keepPictureOf(window, u"send-keys"_s);

        // Shift and then Ctrl are turned on: what that makes is said, in the order they are pressed.
        clickOn(window, theOne(window, u"held:ShiftLeft"_s));
        clickOn(window, theOne(window, u"held:ControlLeft"_s));
        QTRY_VERIFY(saying(window->contentItem(), u"Ctrl+Shift"_s) != nullptr);
        // One key more, from the list, which opens under its button.
        QVERIFY(theOne(window, u"otherKey"_s) != nullptr);
        clickOn(window, theOne(window, u"otherKey"_s));
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 5);
        const QQuickItem* letter = saying(window->contentItem(), u"C"_s);
        QVERIFY(letter != nullptr);
        keepPictureOf(window, u"send-keys-list"_s);
        clickOn(window, letter);
        QTRY_VERIFY(saying(window->contentItem(), u"Ctrl+Shift+C"_s) != nullptr);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        keepPictureOf(window, u"send-keys-chosen"_s);

        // Continue: the program is told what was chosen.
        clickOn(window, theOne(window, u"keysContinue"_s));
        QTRY_COMPARE(keysAsked.count(), 1);
        QCOMPARE(keysAsked[0][0].toString(), u"ControlLeft,ShiftLeft,KeyC"_s);
        // The box stays until the program takes it away. A chip is turned off again.
        QVERIFY(box->property("opened").toBool());
        clickOn(window, theOne(window, u"held:ShiftLeft"_s));
        QTRY_VERIFY(saying(window->contentItem(), u"Ctrl+C"_s) != nullptr);

        // With the keyboard: the list opens from its button, the arrows move in it, Return chooses.
        QVERIFY(tabTo(window->contentItem(), u"otherKey"_s));
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 5);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(saying(window->contentItem(), u"Ctrl+D"_s) != nullptr);
        // Esc in the list closes the list, and not the box.
        QVERIFY(tabTo(window->contentItem(), u"otherKey"_s));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 5);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QVERIFY(box->property("opened").toBool());
        QCOMPARE(closeAsked.count(), 0);

        // What the program finds amiss is said in the box.
        QVariantMap amiss = forKeys;
        amiss[u"problem"_s] = u"Choose at least one key."_s;
        actions.showForm(amiss);
        QTRY_VERIFY(saying(window->contentItem(), u"Choose at least one key."_s) != nullptr);
        QVERIFY(box->property("opened").toBool());

        // Cancel: the program is told, and takes the box away.
        clickOn(window, theOne(window, u"keysCancel"_s));
        QTRY_COMPARE(closeAsked.count(), 1);
        actions.showForm({});
        QTRY_VERIFY(!box->property("opened").toBool());
        // Opened anew, nothing is chosen: what was chosen for one machine is not another's.
        actions.showForm(forKeys);
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_VERIFY(saying(window->contentItem(), u"Nothing chosen"_s) != nullptr);
        clickOn(window, theOne(window, u"keysContinue"_s));
        QTRY_COMPARE(keysAsked.count(), 2);
        QCOMPARE(keysAsked[1][0].toString(), QString());
        // Closed by hand, the program is told.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(closeAsked.count(), 2);
        actions.showForm({});
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theMediaBoxHoldsStillAndOnlyItsListScrolls()
    {
        // A QKVM with one machine's media, more images than the box has room for, and a mount that takes its time.
        QJsonArray images;
        for (int at = 1; at <= 40; ++at) {
            images.append(QJsonObject{{u"id"_s, u"image-%1.iso"_s.arg(at)},
                                      {u"name"_s, u"image-%1.iso"_s.arg(at)},
                                      {u"size"_s, 700'000'000},
                                      {u"partial"_s, false}});
        }
        const QByteArray list =
            QJsonDocument(QJsonObject{{u"images"_s, images},
                                      {u"offers"_s, QJsonObject{{u"drives"_s, QJsonArray{u"cd"_s, u"flash"_s}},
                                                                {u"boot"_s, QJsonArray{}},
                                                                {u"restart"_s, true},
                                                                {u"upload"_s, true},
                                                                {u"where"_s, u"device"_s},
                                                                {u"delete"_s, true}}},
                                      {u"mounted"_s, QJsonValue()}})
                .toJson(QJsonDocument::Compact);
        int mounts = 0;
        qkvm::core::testing::StandIn server([&](const qkvm::core::testing::Asked& asked) {
            if (asked.method == "POST") {
                ++mounts;
                qkvm::core::testing::Answered later{200, R"({"ok": true, "note": null, "mounted": null})"};
                later.afterMs = 1500;
                return later;
            }
            return qkvm::core::testing::Answered{200, list};
        });
        qkvm::core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession(QByteArray(43, 's'));
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        // (Before the actions, whose media box holds a place in it until it goes.)
        qkvm::ui::MediaView::UnderWay underWay;
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"mediaBox"_s);
        QVERIFY(box != nullptr);
        actions.media()->open(&client, u"kvm"_s, u"NAS"_s, u"kvm"_s, &underWay, u"kvm"_s);
        QTRY_VERIFY(box->property("opened").toBool());
        // Where everything stands while nothing is listed yet: so it stands throughout.
        const auto place = [&](const QString& name) {
            const QQuickItem* item = theOne(window, name);
            return item == nullptr ? QRectF() : item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const QStringList still{u"mediaMount"_s,   u"mediaUpload"_s, u"mediaDelete"_s,
                                u"mediaUnmount"_s, u"mediaDrive"_s,  u"mediaBoot"_s,
                                u"mediaRestart"_s, u"mediaList"_s,   u"mediaSaid"_s};
        QQuickItem* whole = qobject_cast<QQuickItem*>(box->property("contentItem").value<QObject*>());
        QVERIFY(whole != nullptr);
        letTheWallComeToRest(window);
        const QRectF boxAt = whole->mapRectToScene(QRectF(0, 0, whole->width(), whole->height()));
        QHash<QString, QRectF> at;
        for (const QString& name : still) {
            at.insert(name, place(name));
            QVERIFY2(!at.value(name).isEmpty(), qPrintable(name));
        }
        const auto holdsStill = [&](const char* when) {
            letTheWallComeToRest(window);
            QCOMPARE(whole->mapRectToScene(QRectF(0, 0, whole->width(), whole->height())), boxAt);
            for (const QString& name : still) {
                QVERIFY2(place(name) == at.value(name), qPrintable(name + u" moved "_s + QString::fromLatin1(when)));
            }
        };
        // Forty images: the list scrolls, and nothing else moves.
        const QQuickItem* listed = theOne(window, u"mediaList"_s);
        QTRY_COMPARE(listed->property("count").toInt(), 40);
        QVERIFY(listed->property("contentHeight").toReal() > listed->height());
        holdsStill("once the images were listed");
        QVERIFY(!theOne(window, u"mediaMount"_s)->isEnabled());
        // One picked by a click: Mount can be had.
        QList<const QQuickItem*> rows;
        named(window->contentItem(), u"mediaPick"_s, rows);
        QVERIFY(!rows.isEmpty());
        clickOn(window, rows.constFirst());
        QTRY_VERIFY(theOne(window, u"mediaMount"_s)->isEnabled());
        clickOn(window, theOne(window, u"mediaMount"_s));
        // Asked first, over the box, naming the machine and the image; nothing sent before the yes.
        QObject* question = window->findChild<QObject*>(u"mediaQuestion"_s);
        QVERIFY(question != nullptr);
        QTRY_VERIFY(question->property("opened").toBool());
        QVERIFY(saying(window->contentItem(),
                       u"NAS gets image-1.iso in its CD-ROM drive, read-only, and nothing else "
                       "is done: pick the KVM's drive in its boot menu at its next start."_s) != nullptr);
        QCOMPARE(mounts, 0);
        keepPictureOf(window, u"media-question"_s);
        clickOn(window, theOne(window, u"mediaYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        // While it is out, nothing in the box takes a click, and nothing moves.
        QTRY_COMPARE(mounts, 1);
        QVERIFY(actions.media()->busy());
        QVERIFY(!theOne(window, u"mediaMount"_s)->isEnabled());
        QVERIFY(!theOne(window, u"mediaDelete"_s)->isEnabled());
        holdsStill("while the mount was out");
        keepPictureOf(window, u"media-box"_s);
        QTRY_VERIFY2(!actions.media()->busy(), qPrintable(actions.media()->said()));
        QTRY_COMPARE(theOne(window, u"mediaSaid"_s)->property("text").toString(), u"Mount image-1.iso: done."_s);
        holdsStill("once it was answered");
        // Closed (Esc, as its way out): the view is closed too.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!actions.media()->opened());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxForATextHandsOverWhatWasTyped()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"typeText"_s);
        QVERIFY(box != nullptr);
        const QSignalSpy textAsked(&actions, &qkvm::ui::ActionsView::textAsked);
        const QSignalSpy closeAsked(&actions, &qkvm::ui::ActionsView::formCloseAsked);
        const QVariantMap forText{{u"kind"_s, u"text"_s}, {u"machine"_s, u"NAS"_s}, {u"problem"_s, QString()}};

        actions.showForm(forText);
        QTRY_VERIFY(box->property("opened").toBool());
        int clipboardReads = 0;
        QString fromClipboard = u"clipboard fixture\nsecond line"_s;
        actions.readClipboardWith([&] {
            ++clipboardReads;
            return fromClipboard;
        });
        QCOMPARE(clipboardReads, 0);
        clickOn(window, theOne(window, u"loadClipboard"_s));
        QCOMPARE(clipboardReads, 1);
        QCOMPARE(theOne(window, u"textBox"_s)->property("text").toString(), fromClipboard);
        QVERIFY(!theOne(window, u"pressEnter"_s)->property("checked").toBool());
        QCOMPARE(textAsked.count(), 0);
        keepPictureOf(window, u"clipboard-preview"_s);
        // The QML text field keeps 2,000 complete supplementary characters, not
        // 2,000 UTF-16 units; an oversized clipboard does not replace that preview.
        const char32_t sign = 0x1F642;
        fromClipboard = QString::fromUcs4(&sign, 1).repeated(2000);
        clickOn(window, theOne(window, u"loadClipboard"_s));
        QCOMPARE(theOne(window, u"textBox"_s)->property("text").toString(), fromClipboard);
        fromClipboard += QString::fromUcs4(&sign, 1);
        clickOn(window, theOne(window, u"loadClipboard"_s));
        QCOMPARE(theOne(window, u"textBox"_s)->property("text").toString().toUcs4().size(), 2000);
        QVERIFY(!actions.form().value(u"problem"_s).toString().isEmpty());
        QCOMPARE(textAsked.count(), 0);
        actions.showForm({});
        QTRY_VERIFY(!box->property("opened").toBool());
        actions.showForm(forText);
        QTRY_VERIFY(box->property("opened").toBool());
        for (const QString& text :
             {u"Type text"_s, u"Text"_s, u"ipconfig /all"_s,
              u"Typed on the connected computer as if on its keyboard (US layout; on a JetKVM, the layout set on "
              u"its own page). It is not saved, and its content is not written to the activity log."_s,
              u"Press Enter afterwards"_s, u"Cancel"_s, u"Continue"_s}) {
            QVERIFY2(saying(window->contentItem(), text) != nullptr, qPrintable(text));
        }
        // The keyboard is in the text from the start.
        const QQuickItem* typed = theOne(window, u"textBox"_s);
        QVERIFY(typed != nullptr);
        QTRY_VERIFY(typed->hasActiveFocus());
        type(window, u"dir c:"_s);
        // Enter there is a line end in the text, not the end of the box.
        QTest::keyClick(window, Qt::Key_Return);
        type(window, u"ver"_s);
        QCOMPARE(typed->property("text").toString(), u"dir c:\nver"_s);
        QVERIFY(box->property("opened").toBool());
        // A text typed and not sent: an update that would close the program waits for it, and names the box.
        QCOMPARE(window->property("unsavedEdit").toString(), u"Type text"_s);
        keepPictureOf(window, u"type-text"_s);

        // Continue: the program is handed the text, and that Enter follows it.
        clickOn(window, theOne(window, u"textContinue"_s));
        QTRY_COMPARE(textAsked.count(), 1);
        QCOMPARE(textAsked[0][0].toString(), u"dir c:\nver"_s);
        QCOMPARE(textAsked[0][1].toBool(), true);
        // Without Enter after it.
        clickOn(window, theOne(window, u"pressEnter"_s));
        clickOn(window, theOne(window, u"textContinue"_s));
        QTRY_COMPARE(textAsked.count(), 2);
        QCOMPARE(textAsked[1][1].toBool(), false);

        // What the program finds amiss is said in the box.
        QVariantMap amiss = forText;
        amiss[u"problem"_s] = u"Enter the text to type."_s;
        actions.showForm(amiss);
        QTRY_VERIFY(saying(window->contentItem(), u"Enter the text to type."_s) != nullptr);

        // Taken away and opened anew: empty, and Enter follows again. What was typed is not kept.
        clickOn(window, theOne(window, u"textCancel"_s));
        QTRY_COMPARE(closeAsked.count(), 1);
        actions.showForm({});
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(typed->property("text").toString(), QString());
        actions.showForm(forText);
        QTRY_VERIFY(box->property("opened").toBool());
        QCOMPARE(typed->property("text").toString(), QString());
        QCOMPARE(window->property("unsavedEdit").toString(), QString());
        clickOn(window, theOne(window, u"textContinue"_s));
        QTRY_COMPARE(textAsked.count(), 3);
        QCOMPARE(textAsked[2][0].toString(), QString());
        QCOMPARE(textAsked[2][1].toBool(), true);

        // Not more letters than a machine is sent at once.
        QVERIFY(tabTo(window->contentItem(), u"textBox"_s));
        QTRY_VERIFY(typed->hasActiveFocus());
        QGuiApplication::clipboard()->setText(QString(2100, u'x'));
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(typed->property("length").toInt(), 2000);
        // Tab leaves the text for the next control: a tab is not typed.
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(!typed->hasActiveFocus());
        QCOMPARE(typed->property("length").toInt(), 2000);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(closeAsked.count(), 2);
        actions.showForm({});
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatWouldBeTypedIsShownOnceMoreInTheQuestion()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* question = window->findChild<QObject*>(u"question"_s);
        QVERIFY(question != nullptr);
        actions.showQuestion({{u"title"_s, u"Type text"_s},
                              {u"text"_s, u"Type this on NAS?"_s},
                              {u"detail"_s, u"shutdown /r /t 0 ⏎"_s},
                              {u"warning"_s, QString()},
                              {u"danger"_s, false}});
        QTRY_VERIFY(question->property("opened").toBool());
        const QQuickItem* detail = theOne(window, u"questionDetail"_s);
        QVERIFY(detail != nullptr);
        QVERIFY(detail->isVisible());
        QVERIFY(says(detail, u"shutdown /r /t 0 ⏎"_s));
        // No warning goes with it, and none is drawn.
        QVERIFY(!theOne(window, u"questionWarning"_s)->isVisible());
        keepPictureOf(window, u"question-text"_s);
        // A question about something else shows no such thing.
        actions.showQuestion(aQuestion());
        QTRY_VERIFY(!detail->isVisible());
        QVERIFY(theOne(window, u"questionWarning"_s)->isVisible());
        actions.showQuestion({});
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whoeverManagesTheQkvmFindsItsWallAsAFileInTheSettings_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("theme");
        for (const QSize& size : {QSize(640, 400), QSize(800, 600), QSize(1280, 800)}) {
            for (const QString& theme : {u"dark"_s, u"light"_s}) {
                QTest::addRow("%d wide, %s", size.width(), qPrintable(theme)) << size << theme;
            }
        }
    }

    // A server's wall as a file is in the settings, as the page has it (app.js, SETTINGS, "Export
    // and import"), for whoever manages that server: listed under a heading of its own, with its two
    // buttons in its first line. Each opens its box over the settings, which it closes back to.
    // Whoever only acts, or only looks, is offered neither: the server would say no. The account's
    // drop has neither any more.
    void whoeverManagesTheQkvmFindsItsWallAsAFileInTheSettings()
    {
        QFETCH(QSize, size);
        QFETCH(QString, theme);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        window->resize(size);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->size(), size);
        QObject* settings = window->findChild<QObject*>(u"settingsBox"_s);
        QObject* exportBox = window->findChild<QObject*>(u"exportBox"_s);
        QObject* importBox = window->findChild<QObject*>(u"importBox"_s);
        QVERIFY(settings != nullptr && exportBox != nullptr && importBox != nullptr);
        const auto sectionKeys = [&] {
            QList<const QQuickItem*> found;
            named(window->contentItem(), u"settingsSection"_s, found);
            QStringList keys;
            for (const QQuickItem* one : std::as_const(found)) {
                if (isSeen(one)) {
                    keys.append(one->property("key").toString());
                }
            }
            return keys;
        };

        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTRY_COMPARE(sectionKeys(), (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s,
                                                 u"about"_s, u"files"_s}));
        // (Under a heading of its own, where the window is wide enough for the list's headings.)
        QCOMPARE(says(window->contentItem(), u"The server's wall"_s), size.width() >= 784);
        QVERIFY(chooseSection(window, u"files"_s));
        QTRY_VERIFY(theOneSeen(window, u"serverExport"_s) != nullptr);
        QVERIFY(theOneSeen(window, u"serverImport"_s) != nullptr);
        QVERIFY(isInside(window, theOneSeen(window, u"serverImport"_s)));
        QVERIFY(says(window->contentItem(), u"The wall as a file: to keep, or to take to another qKVM."_s));
        keepPictureOf(window, u"settings-server-files-%1-%2"_s.arg(size.width()).arg(theme));
        clickOn(window, theOneSeen(window, u"serverExport"_s));
        QTRY_VERIFY(exportBox->property("opened").toBool());
        QVERIFY(settings->property("opened").toBool());
        QVERIFY(!importBox->property("opened").toBool());
        clickOn(window, theOne(window, u"exportCancel"_s));
        QTRY_VERIFY(!exportBox->property("opened").toBool());
        QVERIFY(settings->property("opened").toBool());
        clickOn(window, theOneSeen(window, u"serverImport"_s));
        QTRY_VERIFY(importBox->property("opened").toBool());
        QVERIFY(settings->property("opened").toBool());
        QVERIFY(!exportBox->property("opened").toBool());
        clickOn(window, theOne(window, u"importCancel"_s));
        QTRY_VERIFY(!importBox->property("opened").toBool());
        QVERIFY(settings->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());

        // The account's drop has neither.
        QObject* account = window->findChild<QObject*>(u"accountMenu"_s);
        QVERIFY(account != nullptr);
        clickOn(window, theOne(window, u"accountButton"_s));
        QTRY_VERIFY(account->property("opened").toBool());
        QVERIFY(!says(window->contentItem(), u"Export…"_s));
        QVERIFY(!says(window->contentItem(), u"Import…"_s));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!account->property("opened").toBool());

        // Whoever only looks, or only acts, is offered neither: the server would say no.
        qkvm::ui::SessionView::Shown operates = signedIn();
        operates.manages = false;
        session.show(operates);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTRY_COMPARE(sectionKeys(),
                     (QStringList{u"general"_s, u"video"_s, u"alerts"_s, u"security"_s, u"updates"_s, u"about"_s}));
        QVERIFY(!says(window->contentItem(), u"The server's wall"_s));
        QCOMPARE(seenNamed(window, u"serverExport"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // How a file is chosen in a test: it is kept here, and that one is opened.
    static qkvm::ui::FileDoor doorTo(const QString& savesTo, const QString& opens)
    {
        return {[savesTo](const QString&) { return qkvm::base::Result<QString>(savesTo); },
                [opens] { return qkvm::base::Result<QString>(opens); }};
    }

    // How the notice that was told last is drawn: "ok", "error".
    static QString kindOfTheLastNotice(qkvm::ui::ActionsView& actions)
    {
        const qkvm::ui::Notices* notices = actions.notices();
        return notices->index(notices->rowCount() - 1, 0).data(qkvm::ui::Notices::KindRole).toString();
    }

    void theWallIsWrittenToAFileFromItsBox()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        qkvm::ui::FilesView files;
        const QString kept = m_folder.filePath(u"wall.qkvm"_s);
        QFile::remove(kept);
        files.setDoor(doorTo(kept, {}));
        const QSignalSpy asked(&files, &qkvm::ui::FilesView::exportAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"exportBox"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        // Everything a file can hold, in the page's words, and all of it ticked to begin with.
        for (const QString& label :
             {u"KVMs and servers"_s, u"Saved actions"_s, u"Alert settings"_s, u"Wall settings"_s}) {
            QVERIFY2(says(window->contentItem(), label), qPrintable(label));
        }
        QVERIFY(says(window->contentItem(), u"Passphrase for the file"_s));
        QVERIFY(says(window->contentItem(), u"Your password"_s));
        // (All of it is seen in a window of the usual size: nothing says that there is more.)
        QCOMPARE(seenNamed(window, u"exportScrollMark"_s), 0);
        keepPictureOf(window, u"export"_s);

        // Nothing typed yet: said in the box, and nothing asked.
        clickOn(window, theOne(window, u"exportSave"_s));
        QTRY_COMPARE(seenNamed(window, u"exportProblem"_s), 1);
        QVERIFY(says(window->contentItem(), u"Enter your password."_s));
        QCOMPARE(asked.count(), 0);

        // Without the alert settings; a passphrase, and one's own password.
        clickOn(window, theOne(window, u"exportPart-alerts"_s));
        clickOn(window, theOne(window, u"exportPassphrase"_s));
        type(window, u"a passphrase of mine"_s);
        clickOn(window, theOne(window, u"exportPassword"_s));
        type(window, u"my own"_s);
        clickOn(window, theOne(window, u"exportSave"_s));
        QTRY_COMPARE(asked.count(), 1);
        QCOMPARE(asked.at(0).at(1).toStringList(), (QStringList{u"devices"_s, u"actions"_s, u"wall"_s}));
        QCOMPARE(asked.at(0).at(2).toString(), u"a passphrase of mine"_s);
        QCOMPARE(asked.at(0).at(3).toString(), u"my own"_s);
        QCOMPARE(seenNamed(window, u"exportProblem"_s), 0);
        // While the server works there is nothing to press twice, and what it was asked is not
        // written over.
        QTRY_VERIFY(!theOne(window, u"exportSave"_s)->isEnabled());
        QVERIFY(!theOne(window, u"exportPassphrase"_s)->isEnabled());
        QVERIFY(!theOne(window, u"exportPassword"_s)->isEnabled());
        QVERIFY(!theOne(window, u"exportPart-devices"_s)->isEnabled());

        // Refused: said in the box, which stays.
        files.exported(asked.at(0).at(0).toULongLong(), qkvm::base::Error{"bad_password", "Wrong password"});
        QTRY_VERIFY(says(window->contentItem(), u"Wrong password"_s));
        QVERIFY(box->property("opened").toBool());
        QVERIFY(theOne(window, u"exportSave"_s)->isEnabled());
        QVERIFY(theOne(window, u"exportPassword"_s)->isEnabled());

        // Made and written: the box goes, and what came of it is told in the window's corner, as
        // something that was done.
        clickOn(window, theOne(window, u"exportSave"_s));
        QTRY_COMPARE(asked.count(), 2);
        qkvm::core::WallFile made;
        made.text = "{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}";
        made.sealed = true;
        const int toldBefore = actions.notices()->rowCount();
        files.exported(asked.at(1).at(0).toULongLong(), made);
        QTRY_VERIFY(!box->property("opened").toBool());
        QTRY_VERIFY(
            says(window->contentItem(), u"Exported. The file opens with its passphrase only: keep the two apart."_s));
        QCOMPARE(kindOfTheLastNotice(actions), u"ok"_s);
        // (Told once: by the box that asked, and not by the other one as well.)
        QCOMPARE(actions.notices()->rowCount(), toldBefore + 1);
        QVERIFY(QFile::exists(kept));
        // What was typed is gone with the box: both lines, and what was unticked is ticked again.
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        QCOMPARE(theOne(window, u"exportPassphrase"_s)->property("text").toString(), QString());
        QCOMPARE(theOne(window, u"exportPassword"_s)->property("text").toString(), QString());
        QVERIFY(theOne(window, u"exportPart-alerts"_s)->property("checked").toBool());
        QCOMPARE(seenNamed(window, u"exportProblem"_s), 0);

        // A file that lacks something is told as something to be looked at.
        clickOn(window, theOne(window, u"exportPassword"_s));
        type(window, u"my own"_s);
        clickOn(window, theOne(window, u"exportSave"_s));
        QTRY_COMPARE(asked.count(), 3);
        QCOMPARE(asked.at(2).at(2).toString(), QString());
        made.text = "{\"format\": \"qkvm-export\", \"devices\": []}";
        made.sealed = false;
        made.notOpened = 1;
        files.exported(asked.at(2).at(0).toULongLong(), made);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(kindOfTheLastNotice(actions), u"error"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aSlipOfTheHandDoesNotEndWhatAFileBoxAskedFor()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        qkvm::ui::FilesView files;
        const QString kept = m_folder.filePath(u"given-up.json"_s);
        QFile::remove(kept);
        files.setDoor(doorTo(kept, {}));
        const QSignalSpy asked(&files, &qkvm::ui::FilesView::exportAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"exportBox"_s);
        const QPoint beside(8, window->height() - 8);
        // With nothing under way, a click beside the box closes it, as it does every box.
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        clickOn(window, beside);
        QTRY_VERIFY(!box->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        clickOn(window, theOne(window, u"exportPassword"_s));
        type(window, u"my own"_s);
        clickOn(window, theOne(window, u"exportSave"_s));
        QTRY_COMPARE(asked.count(), 1);
        QVERIFY(files.isBusy());
        // While the server works, neither a click beside it nor Esc closes it.
        clickOn(window, beside);
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(250);
        QVERIFY(box->property("opened").toBool());
        QVERIFY(files.isBusy());
        // "Cancel" does, and means it: what was asked for is given up, and the file that comes
        // all the same is written nowhere and told to nobody.
        const int toldBefore = actions.notices()->rowCount();
        clickOn(window, theOne(window, u"exportCancel"_s));
        QTRY_VERIFY(!box->property("opened").toBool());
        QTRY_VERIFY(!files.isBusy());
        qkvm::core::WallFile made;
        made.text = "{\"format\": \"qkvm-export\", \"devices\": []}";
        files.exported(asked.at(0).at(0).toULongLong(), made);
        QTest::qWait(100);
        QVERIFY(!QFile::exists(kept));
        QCOMPARE(actions.notices()->rowCount(), toldBefore);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // What a file holds, as a server would say it: a device to add, one that is here, and a
    // saved action that types.
    static qkvm::core::FileContents aFileOfAnotherWall()
    {
        qkvm::core::FileContents held;
        held.encrypted = true;
        held.program = u"0.1.0"_s;
        qkvm::core::FileDevice kvm;
        kvm.name = u"Build server"_s;
        kvm.host = u"192.168.1.21"_s;
        kvm.hasPassword = true;
        kvm.isNew = true;
        qkvm::core::FileDevice here;
        here.name = u"NAS"_s;
        here.host = u"192.168.1.22"_s;
        held.devices = QList<qkvm::core::FileDevice>{kvm, here};
        qkvm::core::FileAction restart;
        restart.name = u"Restart now"_s;
        restart.types = true;
        restart.text = u"shutdown /r /t 0"_s;
        restart.enter = true;
        restart.isNew = true;
        held.actions = QList<qkvm::core::FileAction>{restart};
        qkvm::core::FileAlerts alerts;
        alerts.enabled = true;
        held.alerts = alerts;
        held.leftOut = {u"device 3: no address"_s};
        return held;
    }

    void aFileIsTakenInFromItsBox()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        qkvm::ui::FilesView files;
        const QString chosen = m_folder.filePath(u"from-another-wall.qkvm"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}");
        }
        files.setDoor(doorTo({}, chosen));
        const QSignalSpy readAsked(&files, &qkvm::ui::FilesView::readAsked);
        const QSignalSpy takeAsked(&files, &qkvm::ui::FilesView::takeAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"importBox"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        // No file yet: no passphrase to ask for, nothing said of any file, and nothing to take.
        QCOMPARE(seenNamed(window, u"importPassphrase"_s), 0);
        QCOMPARE(seenNamed(window, u"importAbout"_s), 0);
        QCOMPARE(seenNamed(window, u"importNothing"_s), 0);
        QCOMPARE(seenNamed(window, u"importTake"_s), 0);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_VERIFY(says(window->contentItem(), u"Choose a file first."_s));

        clickOn(window, theOne(window, u"importChoose"_s));
        QTRY_VERIFY(says(window->contentItem(), u"from-another-wall.qkvm"_s));
        // It says of itself that it is encrypted: its passphrase is asked for.
        QTRY_COMPARE(seenNamed(window, u"importPassphrase"_s), 1);
        keepPictureOf(window, u"import-chosen"_s);
        clickOn(window, theOne(window, u"importPassphrase"_s));
        type(window, u"of the file"_s);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_COMPARE(readAsked.count(), 1);
        QCOMPARE(readAsked.at(0).at(2).toString(), u"of the file"_s);
        // While the server reads, no other file is chosen, nothing is read twice, and the
        // passphrase it was handed is not written over.
        QTRY_VERIFY(!theOne(window, u"importChoose"_s)->isEnabled());
        QVERIFY(!theOne(window, u"importRead"_s)->isEnabled());
        QVERIFY(!theOne(window, u"importPassphrase"_s)->isEnabled());
        // (Nor is the box closed by a slip of the hand meanwhile.)
        QTest::keyClick(window, Qt::Key_Escape);
        clickOn(window, QPoint(8, window->height() - 8));
        QTest::qWait(250);
        QVERIFY(box->property("opened").toBool());

        // The server has read it: what it holds is said before anything is taken.
        files.fileWasRead(readAsked.at(0).at(0).toULongLong(), aFileOfAnotherWall());
        QTRY_COMPARE(seenNamed(window, u"importTake"_s), 1);
        QCOMPARE(seenNamed(window, u"importRead"_s), 0);
        QVERIFY(theOne(window, u"importChoose"_s)->isEnabled());
        QVERIFY(says(window->contentItem(), u"An encrypted file, made by qKVM 0.1.0."_s));
        QVERIFY(says(window->contentItem(), u"KVMs and servers: 1 to add"_s));
        QVERIFY(says(window->contentItem(), u"1 are on this wall already and stay as they are."_s));
        QVERIFY(says(window->contentItem(), u"Build server · 192.168.1.21"_s));
        QVERIFY(says(window->contentItem(), u"Saved actions: 1 to add"_s));
        QVERIFY(says(window->contentItem(), u"Restart now · types a text, then Enter"_s));
        QVERIFY(says(window->contentItem(), u"1 entry was left out: device 3: no address"_s));
        QCOMPARE(seenNamed(window, u"importNothing"_s), 0);
        // (Its passwords are not in the open: nothing to warn of.)
        QCOMPARE(seenNamed(window, u"importPlainSecrets"_s), 0);
        // What adds is ticked; what replaces a setting of this wall is not.
        QVERIFY(theOne(window, u"importPart-devices"_s)->property("checked").toBool());
        QVERIFY(theOne(window, u"importPart-actions"_s)->property("checked").toBool());
        QVERIFY(!theOne(window, u"importPart-alerts"_s)->property("checked").toBool());
        // What a button will type is shown to whoever asks, and not before; and only an action
        // that types has anything to show.
        QVERIFY(!says(window->contentItem(), u"shutdown /r /t 0"_s));
        QCOMPARE(seenNamed(window, u"importShowTyped"_s), 1);
        clickOn(window, theOneSeen(window, u"importShowTyped"_s));
        QTRY_VERIFY(says(window->contentItem(), u"shutdown /r /t 0"_s));
        // (All of it is seen: nothing says that there is more.)
        QCOMPARE(seenNamed(window, u"importScrollMark"_s), 0);
        keepPictureOf(window, u"import-read"_s);

        // Enter in the passphrase's line reads nothing again: what was unticked stays so.
        clickOn(window, theOne(window, u"importPart-actions"_s));
        clickOn(window, theOne(window, u"importPassphrase"_s));
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(100);
        QCOMPARE(readAsked.count(), 1);
        QVERIFY(files.wasRead());
        QVERIFY(!theOne(window, u"importPart-actions"_s)->property("checked").toBool());
        // The passphrase is changed: what was read with the other is not what would be taken.
        // The file is read again, with that one.
        type(window, u"x"_s);
        QTRY_COMPARE(seenNamed(window, u"importRead"_s), 1);
        QCOMPARE(seenNamed(window, u"importTake"_s), 0);
        QCOMPARE(seenNamed(window, u"importAbout"_s), 0);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(readAsked.count(), 2);
        QCOMPARE(readAsked.at(1).at(2).toString(), u"of the filex"_s);
        files.fileWasRead(readAsked.at(1).at(0).toULongLong(), aFileOfAnotherWall());
        QTRY_COMPARE(seenNamed(window, u"importTake"_s), 1);

        // Without the saved actions: the devices only.
        clickOn(window, theOne(window, u"importPart-actions"_s));
        clickOn(window, theOne(window, u"importTake"_s));
        QTRY_COMPARE(takeAsked.count(), 1);
        QCOMPARE(takeAsked.at(0).at(2).toString(), u"of the filex"_s);
        QCOMPARE(takeAsked.at(0).at(3).toStringList(), (QStringList{u"devices"_s}));
        // While the server takes it in, the box cannot be closed: not by its buttons, not by Esc,
        // not by a click beside it. That cannot be given up, and what came of it is to be told.
        QTRY_VERIFY(!theOne(window, u"importCancel"_s)->isEnabled());
        QVERIFY(!theOne(window, u"importTake"_s)->isEnabled());
        QVERIFY(theOneSeen(window, u"closeBox"_s) != nullptr);
        QVERIFY(!theOneSeen(window, u"closeBox"_s)->isEnabled());
        QTest::keyClick(window, Qt::Key_Escape);
        clickOn(window, QPoint(8, window->height() - 8));
        QTest::qWait(250);
        QVERIFY(box->property("opened").toBool());
        // The server refuses: said in the box, which can be closed again, and what was ticked
        // and what was not is as it was. The same is asked for at the next press.
        files.fileWasTaken(takeAsked.at(0).at(0).toULongLong(),
                           qkvm::base::Error{"busy", "Another file is being worked on just now."});
        QTRY_VERIFY(says(window->contentItem(), u"Another file is being worked on just now."_s));
        QVERIFY(theOne(window, u"importCancel"_s)->isEnabled());
        QVERIFY(theOneSeen(window, u"closeBox"_s)->isEnabled());
        QVERIFY(theOne(window, u"importPart-devices"_s)->property("checked").toBool());
        QVERIFY(!theOne(window, u"importPart-actions"_s)->property("checked").toBool());
        QVERIFY(!theOne(window, u"importPart-alerts"_s)->property("checked").toBool());
        clickOn(window, theOne(window, u"importTake"_s));
        QTRY_COMPARE(takeAsked.count(), 2);
        QCOMPARE(takeAsked.at(1).at(3).toStringList(), (QStringList{u"devices"_s}));
        qkvm::core::Taken taken;
        taken.devices = 1;
        const int toldBefore = actions.notices()->rowCount();
        files.fileWasTaken(takeAsked.at(1).at(0).toULongLong(), taken);
        QTRY_VERIFY(!box->property("opened").toBool());
        QTRY_VERIFY(says(window->contentItem(), u"1 device added"_s));
        QCOMPARE(kindOfTheLastNotice(actions), u"ok"_s);
        // (Told once: by the box that asked, and not by the other one as well.)
        QCOMPARE(actions.notices()->rowCount(), toldBefore + 1);
        // Opened again, the box has nothing of that time: no file, no passphrase.
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        QCOMPARE(theOne(window, u"importPassphrase"_s)->property("text").toString(), QString());
        QCOMPARE(theOne(window, u"importFileName"_s)->property("text").toString(), QString());
        QCOMPARE(seenNamed(window, u"importPassphrase"_s), 0);
        QVERIFY(theOne(window, u"importCancel"_s)->isEnabled());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // A server's file box goes where the window no longer shows a wall it may be used on: also
    // while Settings are not open (a Windows notice's machine shown large closes them, and the box
    // stays). One that is taking a file in, closed so, says that the taking was cut off and what may
    // have come of it, where it is seen: in the corner of the window. And a machine shown large from
    // a notice takes away the boxes that can be closed, and leaves the one that cannot.
    void aServersFileBoxGoesWithTheWallItWasForAndSaysWhatWasCutOff()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QVERIFY(session.manages());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        qkvm::ui::FilesView files;
        const QString chosen = m_folder.filePath(u"cut-off.qkvm"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"format\": \"qkvm-export\", \"sealed\": {\"v\": 1}}");
        }
        files.setDoor(doorTo({}, chosen));
        const QSignalSpy readAsked(&files, &qkvm::ui::FilesView::readAsked);
        const QSignalSpy takeAsked(&files, &qkvm::ui::FilesView::takeAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* exportBox = window->findChild<QObject*>(u"exportBox"_s);
        QObject* importBox = window->findChild<QObject*>(u"importBox"_s);
        QVERIFY(exportBox != nullptr && importBox != nullptr);

        // Export, opened with Settings closed: the role that manages goes, and so does the box.
        QVERIFY(QMetaObject::invokeMethod(exportBox, "open"));
        QTRY_VERIFY(exportBox->property("opened").toBool());
        qkvm::ui::SessionView::Shown operating = signedIn();
        operating.manages = false;
        session.show(operating);
        QTRY_VERIFY(!exportBox->property("opened").toBool());
        session.show(signedIn());

        // Import, read and taking in: a machine shown large from a notice leaves it, and takes the
        // export box away.
        QVERIFY(QMetaObject::invokeMethod(importBox, "open"));
        QTRY_VERIFY(importBox->property("opened").toBool());
        clickOn(window, theOne(window, u"importChoose"_s));
        QTRY_COMPARE(seenNamed(window, u"importPassphrase"_s), 1);
        clickOn(window, theOne(window, u"importPassphrase"_s));
        type(window, u"of the file"_s);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_COMPARE(readAsked.count(), 1);
        files.fileWasRead(readAsked.at(0).at(0).toULongLong(), aFileOfAnotherWall());
        QTRY_COMPARE(seenNamed(window, u"importTake"_s), 1);
        clickOn(window, theOne(window, u"importTake"_s));
        QTRY_COMPARE(takeAsked.count(), 1);
        QTRY_VERIFY(files.isTaking());
        QVERIFY(QMetaObject::invokeMethod(&wall, "revealAsked"));
        QTest::qWait(150);
        QVERIFY(importBox->property("opened").toBool());
        // The role goes while it takes the file in: the box goes, and says so where it is seen.
        const int toldBefore = actions.notices()->rowCount();
        session.show(operating);
        QTRY_VERIFY(!importBox->property("opened").toBool());
        QTRY_COMPARE(actions.notices()->rowCount(), toldBefore + 1);
        QTRY_VERIFY(says(
            window->contentItem(),
            u"Taking the file in was cut off before the server said what came of it. The file may have been taken in all the same: look at the wall before trying again."_s));
        QVERIFY(kindOfTheLastNotice(actions) != u"ok"_s);
        // The server's answer, when it comes, is nobody's: nothing more is said.
        qkvm::core::Taken taken;
        taken.devices = 1;
        files.fileWasTaken(takeAsked.at(0).at(0).toULongLong(), taken);
        QTest::qWait(150);
        QCOMPARE(actions.notices()->rowCount(), toldBefore + 1);
        QVERIFY(!says(window->contentItem(), u"1 device added"_s));

        // A box that can be closed goes where a notice's machine is shown large: export again.
        session.show(signedIn());
        QVERIFY(QMetaObject::invokeMethod(exportBox, "open"));
        QTRY_VERIFY(exportBox->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(&wall, "revealAsked"));
        QTRY_VERIFY(!exportBox->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aFileWithNothingToTakeOffersNoTaking()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        qkvm::ui::FilesView files;
        const QString chosen = m_folder.filePath(u"of-this-wall.json"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"format\": \"qkvm-export\", \"devices\": []}");
        }
        files.setDoor(doorTo({}, chosen));
        const QSignalSpy readAsked(&files, &qkvm::ui::FilesView::readAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"importBox"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        clickOn(window, theOne(window, u"importChoose"_s));
        QTRY_VERIFY(says(window->contentItem(), u"of-this-wall.json"_s));
        // A plain file: no passphrase is asked for.
        QCOMPARE(seenNamed(window, u"importPassphrase"_s), 0);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_COMPARE(readAsked.count(), 1);
        // Everything in it is on this wall already; and it has passwords in the open.
        qkvm::core::FileContents held;
        held.hasSecrets = true;
        qkvm::core::FileDevice here;
        here.name = u"NAS"_s;
        here.host = u"192.168.1.22"_s;
        held.devices = QList<qkvm::core::FileDevice>{here};
        files.fileWasRead(readAsked.at(0).at(0).toULongLong(), held);
        QTRY_VERIFY(says(window->contentItem(), u"KVMs and servers: 0 to add"_s));
        QCOMPARE(seenNamed(window, u"importPlainSecrets"_s), 1);
        QVERIFY(!theOne(window, u"importPart-devices"_s)->isEnabled());
        QVERIFY(!theOne(window, u"importPart-devices"_s)->property("checked").toBool());
        // Nothing can be taken: there is no button for it, and none for reading it again.
        QCOMPARE(seenNamed(window, u"importTake"_s), 0);
        QCOMPARE(seenNamed(window, u"importRead"_s), 0);
        QCOMPARE(seenNamed(window, u"importNothing"_s), 0);

        // A file with no part at all says so.
        clickOn(window, theOne(window, u"importChoose"_s));
        QTRY_COMPARE(seenNamed(window, u"importRead"_s), 1);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_COMPARE(readAsked.count(), 2);
        files.fileWasRead(readAsked.at(1).at(0).toULongLong(), qkvm::core::FileContents{});
        QTRY_COMPARE(seenNamed(window, u"importNothing"_s), 1);
        QVERIFY(says(window->contentItem(), u"There is nothing in this file to take in."_s));
        QCOMPARE(seenNamed(window, u"importPlainSecrets"_s), 0);
        QCOMPARE(seenNamed(window, u"importTake"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxesOfAFileFitTheSmallestWindow()
    {
        // The smallest window the program has is lower than what the boxes hold: the buttons stay
        // in it, what is above them scrolls, and a mark at its edge says that there is more.
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        qkvm::ui::FilesView files;
        const QString chosen = m_folder.filePath(u"long.qkvm"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"format\": \"qkvm-export\", \"sealed\": {}}");
        }
        files.setDoor(doorTo(m_folder.filePath(u"small.json"_s), chosen));
        const QSignalSpy exportAsked(&files, &qkvm::ui::FilesView::exportAsked);
        const QSignalSpy readAsked(&files, &qkvm::ui::FilesView::readAsked);
        const QSignalSpy takeAsked(&files, &qkvm::ui::FilesView::takeAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        window->resize(window->minimumWidth(), window->minimumHeight());
        QTRY_COMPARE(window->height(), 400);
        const auto isInTheWindow = [&](const QString& name) {
            const QQuickItem* item = theOne(window, name);
            if (item == nullptr) {
                return false;
            }
            const QRectF place = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            return QRectF(0, 0, window->width(), window->height()).contains(place);
        };

        // Whether a box lies wholly in the window, from its upper edge to its lower one.
        const auto fitsTheWindow = [&](const QObject* box) {
            const qreal top = box->property("y").toReal();
            return top >= 0 && top + box->property("height").toReal() <= window->height();
        };
        // Whether one thing lies wholly within what another shows of itself.
        const auto isSeenWithin = [&](const QString& name, const QString& around) {
            const QQuickItem* item = theOne(window, name);
            const QQuickItem* frame = theOne(window, around);
            if (item == nullptr || frame == nullptr) {
                return false;
            }
            return frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()))
                .adjusted(-1, -1, 1, 1)
                .contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())));
        };

        QObject* exportBox = window->findChild<QObject*>(u"exportBox"_s);
        QVERIFY(QMetaObject::invokeMethod(exportBox, "open"));
        QTRY_VERIFY(exportBox->property("opened").toBool());
        QTRY_VERIFY(isInTheWindow(u"exportSave"_s));
        QVERIFY(isInTheWindow(u"exportCancel"_s));
        QVERIFY(isInTheWindow(u"closeBox"_s));
        QTRY_VERIFY(fitsTheWindow(exportBox));
        QCOMPARE(seenNamed(window, u"exportScrollMark"_s), 1);
        // The line the keyboard is in is seen. The password's lies below what the box shows at
        // first: gone to with Enter from the passphrase's, it is scrolled to.
        QTRY_VERIFY(isSeenWithin(u"exportPassphrase"_s, u"exportAsked"_s));
        QVERIFY(!isSeenWithin(u"exportPassword"_s, u"exportAsked"_s));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(isSeenWithin(u"exportPassword"_s, u"exportAsked"_s));
        // And stays seen when something said to be wrong takes room from above the buttons.
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(seenNamed(window, u"exportProblem"_s), 1);
        QTRY_VERIFY(isSeenWithin(u"exportPassword"_s, u"exportAsked"_s));
        QVERIFY(isInTheWindow(u"exportSave"_s));
        QTRY_VERIFY(fitsTheWindow(exportBox));
        // Also where it is as long as the server's sentences get: four lines of it.
        type(window, u"my own"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(exportAsked.count(), 1);
        const std::string long4 =
            "The saved passwords are locked just now, and the file cannot be made while they are. Sign in again, "
            "wait until the wall shows its machines, and try once more. If it happens again, qKVM's log says why "
            "they could not be opened, and what was tried before that.";
        files.exported(exportAsked.at(0).at(0).toULongLong(), qkvm::base::Error{"locked", long4});
        QTRY_VERIFY(theOne(window, u"exportProblem"_s)->height() > 60);
        // (Looked at once the box has come to rest: asked at once, it says where it lay before.)
        QTest::qWait(300);
        QVERIFY(fitsTheWindow(exportBox));
        QVERIFY(isInTheWindow(u"exportSave"_s));
        QVERIFY(isInTheWindow(u"exportProblem"_s));
        keepPictureOf(window, u"export-small"_s);
        clickOn(window, theOne(window, u"exportCancel"_s));
        QTRY_VERIFY(!exportBox->property("opened").toBool());

        // The import box with a long file in it, a passphrase asked for and something wrong.
        QObject* importBox = window->findChild<QObject*>(u"importBox"_s);
        QVERIFY(QMetaObject::invokeMethod(importBox, "open"));
        QTRY_VERIFY(importBox->property("opened").toBool());
        // (Nothing to scroll yet: no mark.)
        QCOMPARE(seenNamed(window, u"importScrollMark"_s), 0);
        clickOn(window, theOne(window, u"importChoose"_s));
        QTRY_COMPARE(seenNamed(window, u"importPassphrase"_s), 1);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_COMPARE(readAsked.count(), 1);
        qkvm::core::FileContents held = aFileOfAnotherWall();
        for (int number = 0; number < 40; ++number) {
            qkvm::core::FileDevice more;
            more.name = u"Machine %1"_s.arg(number);
            more.host = u"192.168.1.%1"_s.arg(100 + number);
            more.isNew = true;
            held.devices->append(more);
        }
        files.fileWasRead(readAsked.at(0).at(0).toULongLong(), held);
        QTRY_COMPARE(seenNamed(window, u"importTake"_s), 1);
        QTRY_VERIFY(isInTheWindow(u"importTake"_s));
        QVERIFY(isInTheWindow(u"importCancel"_s));
        QTRY_COMPARE(seenNamed(window, u"importScrollMark"_s), 1);
        QTRY_VERIFY(fitsTheWindow(importBox));
        // With something said to be wrong in it as well, two lines of it: the buttons are still
        // there, and the box is in the window from top to bottom.
        clickOn(window, theOne(window, u"importTake"_s));
        QTRY_COMPARE(takeAsked.count(), 1);
        files.fileWasTaken(takeAsked.at(0).at(0).toULongLong(),
                           qkvm::base::Error{"locked", "The saved passwords are locked just now, and nothing was "
                                                       "taken in. Sign in again and try once more."});
        QTRY_COMPARE(seenNamed(window, u"importProblem"_s), 1);
        QTRY_VERIFY(theOne(window, u"importProblem"_s)->height() > 25);
        QTest::qWait(300);
        QVERIFY(isInTheWindow(u"importTake"_s));
        QVERIFY(fitsTheWindow(importBox));
        QVERIFY(isInTheWindow(u"importProblem"_s));
        QVERIFY(isInTheWindow(u"importPassphrase"_s));
        keepPictureOf(window, u"import-small"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whoeverLeavesTheWallHasNoBoxOfAFileLeftOpen()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        qkvm::ui::FilesView files;
        const QString chosen = m_folder.filePath(u"left-behind.json"_s);
        {
            QFile file(chosen);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"format\": \"qkvm-export\", \"sealed\": {}}");
        }
        files.setDoor(doorTo(m_folder.filePath(u"never.qkvm"_s), chosen));
        const QSignalSpy asked(&files, &qkvm::ui::FilesView::exportAsked);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions, &files);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        qkvm::ui::SessionView::Shown signedOut;
        signedOut.stage = u"signIn"_s;
        signedOut.address = u"192.168.1.10:8787"_s;

        // The export box, with a passphrase and a password typed into it, and a file asked for.
        QObject* exportBox = window->findChild<QObject*>(u"exportBox"_s);
        QVERIFY(QMetaObject::invokeMethod(exportBox, "open"));
        QTRY_VERIFY(exportBox->property("opened").toBool());
        clickOn(window, theOne(window, u"exportPassphrase"_s));
        type(window, u"a passphrase of mine"_s);
        clickOn(window, theOne(window, u"exportPassword"_s));
        type(window, u"my own"_s);
        clickOn(window, theOne(window, u"exportSave"_s));
        QTRY_COMPARE(asked.count(), 1);
        // The server ends the session, or somebody signs out in another window.
        session.show(signedOut);
        QTRY_VERIFY(!exportBox->property("opened").toBool());
        QTRY_VERIFY(!files.isBusy());
        // Whoever signs in after that finds nothing in it of what was typed.
        session.show(signedIn());
        QVERIFY(QMetaObject::invokeMethod(exportBox, "open"));
        QTRY_VERIFY(exportBox->property("opened").toBool());
        QCOMPARE(theOne(window, u"exportPassphrase"_s)->property("text").toString(), QString());
        QCOMPARE(theOne(window, u"exportPassword"_s)->property("text").toString(), QString());
        clickOn(window, theOne(window, u"exportCancel"_s));
        QTRY_VERIFY(!exportBox->property("opened").toBool());

        // The import box, with a file chosen and its passphrase typed.
        QObject* importBox = window->findChild<QObject*>(u"importBox"_s);
        QVERIFY(QMetaObject::invokeMethod(importBox, "open"));
        QTRY_VERIFY(importBox->property("opened").toBool());
        clickOn(window, theOne(window, u"importChoose"_s));
        QTRY_COMPARE(seenNamed(window, u"importPassphrase"_s), 1);
        clickOn(window, theOne(window, u"importPassphrase"_s));
        type(window, u"of the file"_s);
        // Also while it is being taken in, when the box cannot be closed by hand.
        const QSignalSpy readAsked(&files, &qkvm::ui::FilesView::readAsked);
        clickOn(window, theOne(window, u"importRead"_s));
        QTRY_COMPARE(readAsked.count(), 1);
        files.fileWasRead(readAsked.at(0).at(0).toULongLong(), aFileOfAnotherWall());
        QTRY_COMPARE(seenNamed(window, u"importTake"_s), 1);
        clickOn(window, theOne(window, u"importTake"_s));
        QTRY_VERIFY(files.isTaking());
        session.show(signedOut);
        QTRY_VERIFY(!importBox->property("opened").toBool());
        QTRY_COMPARE(files.fileName(), QString());
        QVERIFY(!files.isBusy());
        session.show(signedIn());
        QVERIFY(QMetaObject::invokeMethod(importBox, "open"));
        QTRY_VERIFY(importBox->property("opened").toBool());
        QCOMPARE(theOne(window, u"importPassphrase"_s)->property("text").toString(), QString());
        QCOMPARE(theOne(window, u"importFileName"_s)->property("text").toString(), QString());
        clickOn(window, theOne(window, u"importCancel"_s));
        QTRY_VERIFY(!importBox->property("opened").toBool());

        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aQuestionAboutSeveralNamesThemAndTakesATickAndAPace()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* question = window->findChild<QObject*>(u"question"_s);
        QVERIFY(question != nullptr);
        const QSignalSpy answered(&actions, &qkvm::ui::ActionsView::answered);
        const qkvm::ui::Menu menu = menuOfSeveral();
        QVERIFY(menu.row(u"power:atx_power_long"_s) != nullptr);
        QVERIFY(menu.row(u"keys:ShiftLeft"_s) != nullptr);
        const QVariantMap hold = qkvm::ui::shown(*menu.row(u"power:atx_power_long"_s)->ask, severalMachines());
        const QVariantMap wake = qkvm::ui::shown(*menu.row(u"keys:ShiftLeft"_s)->ask, severalMachines());

        actions.showQuestion(hold);
        QTRY_VERIFY(question->property("opened").toBool());
        // What, and to whom: each machine with where it is and how it is.
        QTRY_COMPARE(seenNamed(window, u"questionTarget"_s), 3);
        for (const QString& text :
             {u"Hold power button on 3 KVMs?"_s, u"A long press cuts the power. Unsaved work is lost."_s, u"NAS"_s,
              u"192.168.1.22"_s, u"Online"_s, u"Build box"_s, u"Offline"_s, u"Rack server"_s, u"Powered off"_s,
              u"In the order shown"_s, u"Cancel"_s}) {
            QVERIFY2(saying(window->contentItem(), text) != nullptr, qPrintable(text));
        }
        const QQuickItem* yes = theOne(window, u"questionYes"_s);
        const QQuickItem* tick = theOne(window, u"questionTick"_s);
        const QQuickItem* pace = theOne(window, u"questionPace"_s);
        QVERIFY(yes != nullptr);
        QVERIFY(tick != nullptr);
        QVERIFY(pace != nullptr);
        QVERIFY(says(yes, u"Run on 3 KVMs"_s));
        QVERIFY(says(tick, u"Yes, do this on all 3 machines"_s));
        // A few at a time, unless something else is chosen.
        QVERIFY(says(pace, u"A few at a time"_s));
        // The yes is not to be had before the tick.
        QVERIFY(!yes->isEnabled());
        clickOn(window, yes);
        QTest::qWait(100);
        QCOMPARE(answered.count(), 0);
        keepPictureOf(window, u"question-several"_s);
        clickOn(window, tick);
        QTRY_VERIFY(yes->isEnabled());
        keepPictureOf(window, u"question-several-ticked"_s);
        clickOn(window, yes);
        QTRY_COMPARE(answered.count(), 1);
        QCOMPARE(answered[0], (QVariantList{true, -1, true}));
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());

        // The next question is answered for itself: nothing is ticked, and nothing chosen.
        actions.showQuestion(hold);
        QTRY_VERIFY(question->property("opened").toBool());
        QTRY_VERIFY(theOne(window, u"questionYes"_s) != nullptr && !theOne(window, u"questionYes"_s)->isEnabled());
        // One after the other, half a minute apart: from the list, which opens under its button.
        clickOn(window, theOne(window, u"questionPace"_s));
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 5);
        const QQuickItem* halfAMinute = saying(window->contentItem(), u"One after the other, 30 seconds apart"_s);
        QVERIFY(halfAMinute != nullptr);
        // Each of them can be read to its end.
        QTRY_VERIFY(choicesCanBeRead(window));
        keepPictureOf(window, u"question-several-pace"_s);
        clickOn(window, halfAMinute);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QTRY_VERIFY(says(theOne(window, u"questionPace"_s), u"One after the other, 30 seconds apart"_s));
        // The keyboard is back at the list's button, in the box: not at a card of the wall that
        // lies under the list, where Enter would enlarge a machine behind the question.
        QTRY_VERIFY(theOne(window, u"questionPace"_s)->hasActiveFocus());
        // The tick with the keyboard.
        QVERIFY(tabTo(window->contentItem(), u"questionTick"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(theOne(window, u"questionYes"_s)->isEnabled());
        clickOn(window, theOne(window, u"questionYes"_s));
        QTRY_COMPARE(answered.count(), 2);
        QCOMPARE(answered[1], (QVariantList{true, 30, true}));
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());

        // Esc is a no, ticked or not.
        actions.showQuestion(hold);
        QTRY_VERIFY(question->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(answered.count(), 3);
        QCOMPARE(answered[2][0].toBool(), false);
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());

        // What harms nobody takes no tick: the yes is to be had at once.
        actions.showQuestion(wake);
        QTRY_VERIFY(question->property("opened").toBool());
        QTRY_COMPARE(seenNamed(window, u"questionTarget"_s), 3);
        QCOMPARE(seenNamed(window, u"questionTick"_s), 0);
        QVERIFY(theOne(window, u"questionYes"_s) != nullptr);
        QVERIFY(theOne(window, u"questionYes"_s)->isEnabled());
        clickOn(window, theOne(window, u"questionYes"_s));
        QTRY_COMPARE(answered.count(), 4);
        QCOMPARE(answered[3], (QVariantList{true, -1, false}));
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());

        // About one machine nobody is listed, nothing is ticked, and no pace is asked for.
        actions.showQuestion(aQuestion());
        QTRY_VERIFY(question->property("opened").toBool());
        QTRY_VERIFY(saying(window->contentItem(), u"Hold power button on NAS?"_s) != nullptr);
        QCOMPARE(seenNamed(window, u"questionTargets"_s), 0);
        QCOMPARE(seenNamed(window, u"questionTarget"_s), 0);
        QCOMPARE(seenNamed(window, u"questionTick"_s), 0);
        QCOMPARE(seenNamed(window, u"questionPace"_s), 0);
        QVERIFY(theOne(window, u"questionYes"_s)->isEnabled());
        actions.showQuestion({});
        QTRY_VERIFY(!question->property("opened").toBool());

        // Many machines, in the smallest window there is: the list of them scrolls, and the box
        // with its yes and its no stays whole inside the window.
        QList<qkvm::ui::Target> many;
        QList<qkvm::ui::MenuOf> ofMany;
        for (int number = 1; number <= 40; ++number) {
            many.append({u"Node %1"_s.arg(number), u"192.168.1.%1"_s.arg(100 + number), u"online"_s, u"Online"_s});
            qkvm::ui::MenuOf one;
            one.machine.id = u"n%1"_s.arg(number);
            one.machine.name = many.constLast().name;
            qkvm::core::Status status;
            status.state = u"online"_s;
            status.powerIsKnown = true;
            one.status = status;
            ofMany.append(one);
        }
        const qkvm::ui::Menu menuOfMany = qkvm::ui::menuForSeveral(ofMany);
        QVERIFY(menuOfMany.row(u"power:atx_power_long"_s) != nullptr);
        window->setWidth(window->minimumWidth());
        window->setHeight(window->minimumHeight());
        actions.showQuestion(qkvm::ui::shown(*menuOfMany.row(u"power:atx_power_long"_s)->ask, many));
        QTRY_VERIFY(question->property("opened").toBool());
        QTRY_VERIFY(theOne(window, u"questionYes"_s) != nullptr);
        QTRY_VERIFY(isInside(window, theOne(window, u"questionYes"_s)));
        QVERIFY(isInside(window, theOne(window, u"questionNo"_s)));
        QVERIFY(isInside(window, theOne(window, u"questionText"_s)));
        QVERIFY(isInside(window, theOne(window, u"questionTick"_s)));
        QVERIFY(says(theOne(window, u"questionYes"_s), u"Run on 40 KVMs"_s));
        // (Not all forty are made for a list that shows a few.)
        QVERIFY(seenNamed(window, u"questionTarget"_s) >= 2);
        keepPictureOf(window, u"question-several-small"_s);
        // The list of how to go about it has no room under its button here: it is inside the window
        // all the same, with all it offers.
        clickOn(window, theOne(window, u"questionPace"_s));
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 5);
        QTRY_VERIFY(choicesCanBeRead(window));
        keepPictureOf(window, u"question-several-small-pace"_s);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QVERIFY(question->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // What a line of a form is for is written above it as the page writes it (its `.field > span`):
    // as it is spelled, in the text's colour and size, a little heavier. Not in small grey capitals,
    // which made "Address of your qKVM" read "ADDRESS OF YOUR QKVM". In both themes, for a line and
    // for a box of several lines.
    void aLineOfAFormSaysWhatItIsForAsThePageWritesIt()
    {
        for (const Qt::ColorScheme scheme : {Qt::ColorScheme::Dark, Qt::ColorScheme::Light}) {
            qkvm::ui::AboutModel about;
            qkvm::ui::WallView wall;
            wall.machines()->setTiles(qkvm::ui::pretendWall());
            qkvm::ui::SessionView session;
            qkvm::ui::ActionsView actions;
            actions.setAllowed(true);
            QQmlApplicationEngine engine;
            QQuickWindow* window = load(engine, about, session, wall, &actions);
            QVERIFY(window != nullptr);
            auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
            QVERIFY(colours != nullptr);
            QVERIFY(colours->setProperty("scheme", static_cast<int>(scheme)));
            QVERIFY(bringUp(window));
            const QColor text = colours->property("text").value<QColor>();
            const auto asThePageWritesIt = [&](const QQuickItem* label) {
                if (label == nullptr) {
                    return QString(u"no such line"_s);
                }
                const QFont font = label->property("font").value<QFont>();
                const QColor colour = label->property("color").value<QColor>();
                if (font.capitalization() != QFont::MixedCase || !qFuzzyIsNull(font.letterSpacing()) ||
                    font.pixelSize() != 13 || font.weight() != QFont::Medium || colour != text) {
                    return u"capitals %1, spaced %2, %3 px, weight %4, %5"_s.arg(font.capitalization())
                        .arg(font.letterSpacing())
                        .arg(font.pixelSize())
                        .arg(font.weight())
                        .arg(colour.name());
                }
                return QString();
            };

            // What is typed in, or a box's sentence, stands 6 px under what it is for, as on the page.
            const auto gapUnder = [](const QQuickItem* label) {
                const QList<QQuickItem*> beside = label->parentItem()->childItems();
                for (qsizetype at = 0; at + 1 < beside.size(); ++at) {
                    if (beside.at(at) == label) {
                        return beside.at(at + 1)->y() - (label->y() + label->height());
                    }
                }
                return -1.0;
            };

            // A line: which qKVM, before anybody is signed in.
            QCOMPARE(asThePageWritesIt(saying(window->contentItem(), u"Address of your qKVM"_s)), QString());
            QCOMPARE(gapUnder(saying(window->contentItem(), u"Address of your qKVM"_s)), 6.0);

            // A box of several lines: the text to be typed on a machine.
            session.show(signedIn());
            QObject* box = window->findChild<QObject*>(u"typeText"_s);
            QVERIFY(box != nullptr);
            actions.showForm({{u"kind"_s, u"text"_s}, {u"machine"_s, u"NAS"_s}, {u"problem"_s, QString()}});
            QTRY_VERIFY(box->property("opened").toBool());
            QCOMPARE(asThePageWritesIt(saying(box->property("contentItem").value<QQuickItem*>(), u"Text"_s)),
                     QString());
            QCOMPARE(gapUnder(saying(box->property("contentItem").value<QQuickItem*>(), u"Text"_s)), 6.0);
            QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
        }
    }

    void whatIsTypedIntoALineThatHidesItIsShownWhileItsEyeIsOpenAndNoLongerOnceItIsSent()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy asked(&m_adding, &qkvm::ui::AddKvmView::addAsked);
        const auto* button = window->findChild<QQuickItem*>(u"addButton"_s);
        QVERIFY(button != nullptr);
        clickOn(window, button);
        QObject* box = window->findChild<QObject*>(u"addKvm"_s);
        QVERIFY(box != nullptr);
        QTRY_VERIFY(box->property("opened").toBool());
        const auto* address = window->findChild<QQuickItem*>(u"kvmAddress"_s);
        const auto* password = window->findChild<QQuickItem*>(u"kvmPassword"_s);
        const auto* close = window->findChild<QQuickItem*>(u"kvmClose"_s);
        QVERIFY(address != nullptr && password != nullptr && close != nullptr);

        // The line for the password has an eye; a line that hides nothing has none to be seen.
        const auto* eye = password->findChild<QQuickItem*>(u"eye"_s);
        const auto* line = password->findChild<QQuickItem*>(u"line"_s);
        const auto* noEye = address->findChild<QQuickItem*>(u"eye"_s);
        QVERIFY2(eye != nullptr && line != nullptr, "the line for the password has no eye");
        QVERIFY(eye->isVisible());
        QVERIFY(noEye != nullptr);
        QVERIFY(!noEye->isVisible());
        QCOMPARE(eye->property("focusPolicy").toInt(), static_cast<int>(Qt::NoFocus));

        // What is typed there is not read off the screen.
        type(window, u"192.168.1.20"_s);
        QTest::keyClick(window, Qt::Key_Return);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        const auto drawn = [line] { return line->property("displayText").toString(); };
        QVERIFY(drawn() != u"a password"_s);
        QCOMPARE(drawn().size(), 10);
        // The eye is pressed: it is, and the keyboard is still in that line.
        clickOn(window, eye);
        QVERIFY(password->property("shown").toBool());
        QCOMPARE(drawn(), u"a password"_s);
        type(window, u"!"_s);
        QCOMPARE(drawn(), u"a password!"_s);
        keepPictureOf(window, u"add-kvm-password-shown"_s);
        // Pressed again: hidden.
        clickOn(window, eye);
        QVERIFY(!password->property("shown").toBool());
        QVERIFY(drawn() != u"a password!"_s);

        // On show, and sent: it is on show no longer, as on the page. What was typed went as it
        // was typed.
        clickOn(window, eye);
        QCOMPARE(drawn(), u"a password!"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked[0][2].toString(), u"a password!"_s);
        QVERIFY(!password->property("shown").toBool());
        QVERIFY(drawn() != u"a password!"_s);

        // On show, and the box is left: when it is opened again, nothing is on show.
        clickOn(window, eye);
        QVERIFY(password->property("shown").toBool());
        clickOn(window, close);
        QTRY_VERIFY(!box->property("opened").toBool());
        QTRY_VERIFY(!password->property("shown").toBool());
    }

    void aKvmIsAddedFromItsBox()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy asked(&m_adding, &qkvm::ui::AddKvmView::addAsked);
        const QSignalSpy pageAsked(&session, &qkvm::ui::SessionView::pageAsked);

        // The bar's button opens the box here, and no page of a server's.
        const auto* button = window->findChild<QQuickItem*>(u"addButton"_s);
        QVERIFY(button != nullptr);
        QVERIFY(button->isVisible());
        clickOn(window, button);
        QObject* box = window->findChild<QObject*>(u"addKvm"_s);
        QVERIFY(box != nullptr);
        QTRY_VERIFY(box->property("opened").toBool());
        QCOMPARE(pageAsked.count(), 0);
        QVERIFY(says(window->contentItem(), u"Add a KVM"_s));
        const auto* address = window->findChild<QQuickItem*>(u"kvmAddress"_s);
        const auto* user = window->findChild<QQuickItem*>(u"kvmUser"_s);
        const auto* password = window->findChild<QQuickItem*>(u"kvmPassword"_s);
        const auto* group = window->findChild<QQuickItem*>(u"kvmGroup"_s);
        const auto* add = window->findChild<QQuickItem*>(u"kvmAdd"_s);
        const auto* close = window->findChild<QQuickItem*>(u"kvmClose"_s);
        QVERIFY(address != nullptr && user != nullptr && password != nullptr && group != nullptr);
        QVERIFY(add != nullptr && close != nullptr);
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        keepPictureOf(window, u"add-kvm"_s);

        // The keyboard is in the address's line; Enter there goes on to the password, and Enter
        // there asks: with what was typed, and with nothing where nothing was.
        type(window, u"192.168.1.20"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(asked.count(), 0);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        // What is typed there is not read off the screen.
        QVERIFY(password->property("secret").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked[0][0].toString(), u"192.168.1.20"_s);
        QCOMPARE(asked[0][1].toString(), QString());
        QCOMPARE(asked[0][2].toString(), u"a password"_s);
        QCOMPARE(asked[0][3].toString(), QString());

        // While the KVM is asked: said so, nothing more is asked, and the box is not left by a slip
        // of the hand.
        const QSignalSpy giveUpAsked(&m_adding, &qkvm::ui::AddKvmView::giveUpAsked);
        m_adding.began();
        QTRY_VERIFY(says(window->contentItem(), u"Checking…"_s));
        QVERIFY(!add->isEnabled());
        QVERIFY(!address->isEnabled());
        // (Neither by Esc or a click beside it, nor by the way out in its corner.)
        QVERIFY(!box->property("closesBeside").toBool());
        QVERIFY(!box->property("canClose").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Escape);
        QCOMPARE(asked.count(), 1);
        QVERIFY(box->property("opened").toBool());
        keepPictureOf(window, u"add-kvm-checking"_s);
        // It is given up with the button that says so, which does not close the box: a KVM that
        // answers a byte at a time is not waited for to the end.
        QVERIFY(close->isEnabled());
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        clickOn(window, close);
        QCOMPARE(giveUpAsked.count(), 1);
        QVERIFY(box->property("opened").toBool());
        m_adding.gaveUp();
        QTRY_VERIFY(!says(window->contentItem(), u"Checking…"_s));
        // What was typed for it stands, to be tried again, and the keyboard is back in the line it
        // was in (the click on the button took it away).
        QCOMPARE(address->property("text").toString(), u"192.168.1.20"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        QVERIFY(add->isEnabled());
        QTRY_VERIFY(password->property("hasKeyboard").toBool());
        m_adding.began();

        // It was not added: why, and what was typed stands as it was, to be put right.
        m_adding.refused(u"The KVM refused this user name and password."_s);
        QTRY_VERIFY(says(window->contentItem(), u"The KVM refused this user name and password."_s));
        QVERIFY(!says(window->contentItem(), u"Checking…"_s));
        QVERIFY(add->isEnabled());
        QCOMPARE(address->property("text").toString(), u"192.168.1.20"_s);
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        keepPictureOf(window, u"add-kvm-refused"_s);

        // A user name and a group, where they are typed, go along.
        QVERIFY(password->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Backtab);
        QVERIFY(user->property("hasKeyboard").toBool());
        type(window, u"operator"_s);
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(password->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(group->property("hasKeyboard").toBool());
        type(window, u"Rack 1"_s);
        clickOn(window, add);
        QCOMPARE(asked.count(), 2);
        QCOMPARE(asked[1][0].toString(), u"192.168.1.20"_s);
        QCOMPARE(asked[1][1].toString(), u"operator"_s);
        QCOMPARE(asked[1][2].toString(), u"a password"_s);
        QCOMPARE(asked[1][3].toString(), u"Rack 1"_s);

        // It is on the wall: said, the complaint is gone, and the box is ready for the next one,
        // which is signed in to as this one was: the address's line is empty and has the keyboard.
        m_adding.began();
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        m_adding.took(u"Build server added"_s, u"AB:CD:EF"_s);
        QTRY_VERIFY(says(window->contentItem(), u"Build server added"_s));
        QVERIFY(!says(window->contentItem(), u"The KVM refused this user name and password."_s));
        // And what it is known by from now on, to be compared with what the KVM says of itself.
        QVERIFY(says(window->contentItem(), u"Remembered SHA-256: AB:CD:EF"_s));
        QCOMPARE(address->property("text").toString(), QString());
        QCOMPARE(user->property("text").toString(), u"operator"_s);
        QCOMPARE(close->property("text").toString(), u"Done"_s);
        keepPictureOf(window, u"add-kvm-added"_s);
        type(window, u"192.168.1.21"_s);
        QCOMPARE(address->property("text").toString(), u"192.168.1.21"_s);
        // That one is refused: its complaint stands alone, and not beside the last one's success;
        // and what was typed for it stands. (While it is asked, the way out is the giving up.)
        m_adding.began();
        QTRY_COMPARE(close->property("text").toString(), u"Cancel"_s);
        m_adding.refused(u"Device is unreachable"_s);
        QTRY_VERIFY(says(window->contentItem(), u"Device is unreachable"_s));
        QVERIFY(!says(window->contentItem(), u"Build server added"_s));
        QVERIFY(!says(window->contentItem(), u"Remembered SHA-256: AB:CD:EF"_s));
        QCOMPARE(address->property("text").toString(), u"192.168.1.21"_s);
        QCOMPARE(close->property("text").toString(), u"Done"_s);

        // The box is left: nothing that was typed is kept, the password least of all.
        clickOn(window, close);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(address->property("text").toString(), QString());
        QCOMPARE(user->property("text").toString(), QString());
        QCOMPARE(group->property("text").toString(), QString());
        QCOMPARE(m_adding.added(), QString());
        QCOMPARE(m_adding.count(), 0);
        QCOMPARE(asked.count(), 2);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxThatAddsAKvmGoesWithThisPcsOwnWall()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"addKvm"_s);
        const auto* password = window->findChild<QQuickItem*>(u"kvmPassword"_s);
        QVERIFY(box != nullptr && password != nullptr);
        const QSignalSpy giveUpAsked(&m_adding, &qkvm::ui::AddKvmView::giveUpAsked);

        // Another window went on to a server, and this one shows that server's wall now: the box
        // is not left open over it, with a password typed into it.
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        type(window, u"192.168.1.20"_s);
        QTest::keyClick(window, Qt::Key_Return);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        session.show(signedIn());
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(giveUpAsked.count(), 0);

        // Nor over the screen that asks for a server; and a KVM that was being asked is given up.
        session.show(atItsOwnWall());
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        m_adding.began();
        session.show({});
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(giveUpAsked.count(), 1);
        m_adding.gaveUp();
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxThatAddsAKvmFitsTheSmallestWindow()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->resize(window->minimumWidth(), window->minimumHeight());
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"addKvm"_s);
        QVERIFY(box != nullptr);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        // With the longest it ever says: a complaint of several lines.
        m_adding.refused(u"This KVM shows a certificate that nobody has vouched for and that is not the one this PC "
                         "remembers for it. Nothing was sent to it. Compare the two before you go on."_s);
        QTRY_VERIFY(says(window->contentItem(), m_adding.problem()));
        letTheWallComeToRest(window);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        // What came of it and the buttons are in sight whatever the window's size.
        for (const QString& name : {u"kvmProblem"_s, u"kvmClose"_s, u"kvmAdd"_s, u"kvmLines"_s}) {
            const auto* item = window->findChild<QQuickItem*>(name);
            QVERIFY2(item != nullptr, qPrintable(name));
            QVERIFY2(item->isVisible(), qPrintable(name));
            QVERIFY2(inside.contains(placeOf(item)), qPrintable(name));
        }
        // The lines to type into are scrolled where there is no room for all: each is in sight when
        // the keyboard comes to it, forwards and back.
        const auto* lines = window->findChild<QQuickItem*>(u"kvmLines"_s);
        QVERIFY(lines->property("contentHeight").toReal() > lines->height());
        const QStringList inTurn{u"kvmAddress"_s, u"kvmUser"_s, u"kvmPassword"_s, u"kvmGroup"_s};
        for (int at = 0; at < inTurn.size() * 2 - 1; ++at) {
            const QString& name = inTurn.at(at < inTurn.size() ? at : inTurn.size() * 2 - 2 - at);
            const auto* line = window->findChild<QQuickItem*>(name);
            QVERIFY2(line != nullptr, qPrintable(name));
            QTRY_VERIFY2(line->property("hasKeyboard").toBool(), qPrintable(name));
            QTRY_VERIFY2(placeOf(lines).adjusted(0, -1, 0, 1).contains(placeOf(line)), qPrintable(name));
            QTest::keyClick(window, at < inTurn.size() - 1 ? Qt::Key_Tab : Qt::Key_Backtab);
        }
        keepPictureOf(window, u"add-kvm-smallest"_s);
        m_adding.reset();
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aKvmIsChangedFromItsBox()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy chosen(&actions, &qkvm::ui::ActionsView::chosen);
        const QSignalSpy openAsked(&m_editing, &qkvm::ui::EditKvmView::openAsked);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QSignalSpy giveUpAsked(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked);
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        QVERIFY(!box->property("opened").toBool());

        // The menu of a KVM of this PC's own wall has the row that leads to the box, among those
        // that lead elsewhere; chosen, the program is told which.
        qkvm::ui::MenuOf of;
        of.machine.id = u"0a1b2c3d"_s;
        of.machine.name = u"NAS"_s;
        of.machine.host = u"192.168.1.22"_s;
        of.machine.kind = u"kvmd"_s;
        of.machine.own = true;
        of.status = qkvm::core::Status{};
        of.status->state = u"online"_s;
        actions.showMenu(qkvm::ui::shown(qkvm::ui::menuFor(of)));
        const QObject* menu = window->findChild<QObject*>(u"machineMenu"_s);
        QTRY_VERIFY(menu->property("opened").toBool());
        const QQuickItem* row = theOne(window, u"row:edit"_s);
        QVERIFY(row != nullptr);
        QVERIFY(saying(window->contentItem(), u"Edit…"_s) != nullptr);
        keepPictureOf(window, u"machine-menu-own"_s);
        clickOn(window, row);
        QTRY_COMPARE(chosen.count(), 1);
        QCOMPARE(chosen[0][0].toString(), u"edit"_s);
        // The program closes the menu and asks for the box; the window passes that to its own
        // box's view, and the program fills the box and opens it.
        actions.showMenu({});
        emit actions.editAsked(u"0a1b2c3d"_s, false);
        QCOMPARE(openAsked.count(), 1);
        QCOMPARE(openAsked[0][0].toString(), u"0a1b2c3d"_s);
        QCOMPARE(openAsked[0][1].toBool(), false);
        QVERIFY(!box->property("opened").toBool());
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_VERIFY(!menu->property("opened").toBool());

        // The KVM as it is on the wall stands in its lines; no password does, and the line for
        // one says that an empty one changes nothing.
        QVERIFY(says(window->contentItem(), u"Edit \"NAS\""_s));
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* address = window->findChild<QQuickItem*>(u"editAddress"_s);
        const auto* user = window->findChild<QQuickItem*>(u"editUser"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        const auto* group = window->findChild<QQuickItem*>(u"editGroup"_s);
        const auto* remove = window->findChild<QQuickItem*>(u"editRemove"_s);
        const auto* review = window->findChild<QQuickItem*>(u"editReview"_s);
        const auto* cancel = window->findChild<QQuickItem*>(u"editCancel"_s);
        const auto* save = window->findChild<QQuickItem*>(u"editSave"_s);
        QVERIFY(name != nullptr && address != nullptr && user != nullptr && password != nullptr && group != nullptr);
        QVERIFY(remove != nullptr && review != nullptr && cancel != nullptr && save != nullptr);
        QCOMPARE(name->property("text").toString(), u"NAS"_s);
        QCOMPARE(address->property("text").toString(), u"192.168.1.22"_s);
        QCOMPARE(user->property("text").toString(), u"admin"_s);
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(password->property("placeholder").toString(), u"Unchanged"_s);
        QVERIFY(password->property("secret").toBool());
        QCOMPARE(group->property("text").toString(), u"Rack A"_s);
        for (const QString& text :
             {u"Name"_s, u"Address"_s, u"Type: GL.iNet Comet / PiKVM"_s, u"User name"_s, u"Password"_s,
              u"Leave empty to keep the saved one."_s, u"Group"_s, u"Remembered SHA-256: "_s + writtenOne()}) {
            QVERIFY2(says(window->contentItem(), text), qPrintable(text));
        }
        // Its buttons, all of one height: the way off the wall at the left, the one that saves at
        // the right.
        QVERIFY(remove->isVisible() && review->isVisible() && cancel->isVisible() && save->isVisible());
        QCOMPARE(remove->property("text").toString(), u"Remove"_s);
        QCOMPARE(review->property("text").toString(), u"Review certificate"_s);
        QCOMPARE(cancel->property("text").toString(), u"Cancel"_s);
        QCOMPARE(save->property("text").toString(), u"Save"_s);
        QVERIFY(remove->property("danger").toBool());
        QVERIFY(save->property("primary").toBool());
        QVERIFY(!review->property("primary").toBool() && !cancel->property("primary").toBool());
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        letTheWallComeToRest(window);
        QVERIFY(placeOf(remove).right() < placeOf(review).left());
        QVERIFY(placeOf(review).right() < placeOf(cancel).left());
        QVERIFY(placeOf(cancel).right() < placeOf(save).left());
        for (const QQuickItem* button : {remove, review, cancel, save}) {
            QCOMPARE(placeOf(button).top(), placeOf(save).top());
            QCOMPARE(button->height(), save->height());
        }
        keepPictureOf(window, u"edit-kvm"_s);

        // The keyboard is in the name's line. Another name is typed over the one that stands
        // there, and Enter saves: with what stands in the lines, and no password.
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        type(window, u"Storage"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked[0][0].toString(), u"0a1b2c3d"_s);
        QCOMPARE(saveAsked[0][1].toString(), u"Storage"_s);
        QCOMPARE(saveAsked[0][2].toString(), u"192.168.1.22"_s);
        QCOMPARE(saveAsked[0][3].toString(), u"admin"_s);
        QCOMPARE(saveAsked[0][4].toString(), QString());
        QCOMPARE(saveAsked[0][5].toString(), u"Rack A"_s);

        // While the KVM is asked: said so at the button that asked, nothing more is asked, and the
        // box is not left by a slip of the hand. The buttons stay where they are.
        const QRectF cancelWas = placeOf(cancel);
        const QRectF saveWas = placeOf(save);
        m_editing.began(u"saving"_s);
        QTRY_COMPARE(save->property("text").toString(), u"Checking…"_s);
        QCOMPARE(review->property("text").toString(), u"Review certificate"_s);
        QVERIFY(!save->isEnabled() && !review->isEnabled() && !remove->isEnabled());
        QVERIFY(!name->isEnabled() && !password->isEnabled());
        QVERIFY(!box->property("closesBeside").toBool());
        QVERIFY(!box->property("canClose").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Escape);
        QCOMPARE(saveAsked.count(), 1);
        QVERIFY(box->property("opened").toBool());
        letTheWallComeToRest(window);
        QCOMPARE(placeOf(cancel), cancelWas);
        QCOMPARE(placeOf(save), saveWas);
        // (The pacing's third reading.) Its sign-in waits for an earlier one to that KVM to be answered: said plainly,
        // in the window's corner, once a saving, and nothing of the box moves for it.
        const QString checking = save->property("text").toString();
        m_editing.signInWaits();
        m_editing.signInWaits();
        QTRY_COMPARE(toldBy(actions).size(), 1);
        QVERIFY2(toldBy(actions).constFirst().endsWith(
                     u": Waiting for an earlier sign-in to this KVM to be answered before this one is sent."_s),
                 qPrintable(toldBy(actions).constFirst()));
        QVERIFY(m_editing.waitsForAnotherSignIn());
        letTheWallComeToRest(window);
        QCOMPARE(save->property("text").toString(), checking);
        QCOMPARE(placeOf(cancel), cancelWas);
        QCOMPARE(placeOf(save), saveWas);
        keepPictureOf(window, u"edit-kvm-checking"_s);
        // It is given up with the button that says so, which does not close the box.
        QVERIFY(cancel->isEnabled());
        clickOn(window, cancel);
        QCOMPARE(giveUpAsked.count(), 1);
        QVERIFY(box->property("opened").toBool());
        m_editing.gaveUp();
        QTRY_COMPARE(save->property("text").toString(), u"Save"_s);
        // What was typed stands, and the keyboard is back in the line it was in (the click on the
        // button took it away).
        QCOMPARE(name->property("text").toString(), u"Storage"_s);
        QTRY_VERIFY(name->property("hasKeyboard").toBool());

        // A password is typed with it, and refused: why is said, and what was typed stands as it
        // was, to be put right, with the keyboard in the line it was typed in.
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(password->property("hasKeyboard").toBool());
        type(window, u"not it"_s);
        clickOn(window, save);
        QCOMPARE(saveAsked.count(), 2);
        QCOMPARE(saveAsked[1][4].toString(), u"not it"_s);
        m_editing.began(u"saving"_s);
        m_editing.refused(u"Login rejected - check the username and password"_s);
        // (Looked for in the box: a card of the wall behind it says the same of its own KVM.)
        const auto* problem = window->findChild<QQuickItem*>(u"editProblem"_s);
        QVERIFY(problem != nullptr);
        QTRY_VERIFY(isSeen(problem));
        QCOMPARE(problem->property("text").toString(), u"Login rejected - check the username and password"_s);
        QCOMPARE(save->property("text").toString(), u"Save"_s);
        QVERIFY(save->isEnabled());
        QCOMPARE(name->property("text").toString(), u"Storage"_s);
        QCOMPARE(password->property("text").toString(), u"not it"_s);
        QTRY_VERIFY(password->property("hasKeyboard").toBool());
        QVERIFY(box->property("opened").toBool());
        keepPictureOf(window, u"edit-kvm-refused"_s);

        // The next try says nothing of the last one. It is saved: the box closes, that is told in
        // the window's corner as the page tells it, and nothing that was typed is kept, the
        // password least of all.
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        type(window, u"secret"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(saveAsked.count(), 3);
        QCOMPARE(saveAsked[2][4].toString(), u"secret"_s);
        m_editing.began(u"saving"_s);
        QTRY_VERIFY(!isSeen(problem));
        m_editing.saved(u"Storage"_s);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(toldBy(actions).filter(u"ok: "_s), QStringList{u"ok: Storage saved"_s});
        QTRY_VERIFY(saying(window->contentItem(), u"Storage saved"_s) != nullptr);
        for (const QQuickItem* line : {name, address, user, password, group}) {
            QCOMPARE(line->property("text").toString(), QString());
        }
        QCOMPARE(m_editing.machine(), QString());
        QCOMPARE(m_editing.name(), QString());
        QCOMPARE(m_editing.certificate(), QString());
        QVERIFY(!m_editing.busy());
        QCOMPARE(giveUpAsked.count(), 1);
        keepPictureOf(window, u"edit-kvm-saved"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aKvmIsTakenOffTheWallFromItsBoxOnceThatWasAskedAbout()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy removeAsked(&m_editing, &qkvm::ui::EditKvmView::removeAsked);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);

        // A KVM that is spoken to over plain HTTP: its address is typed back with that before it,
        // and the box says what that means, in the place of a certificate. It has none to review.
        qkvm::ui::EditKvmView::Shown plain = aKvmOfTheWall();
        plain.address = u"http://192.168.1.22:8080"_s;
        plain.certificate.clear();
        plain.https = false;
        plain.overPlainHttp = true;
        plain.group.clear();
        m_editing.show(plain, false);
        QTRY_VERIFY(box->property("opened").toBool());
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* address = window->findChild<QQuickItem*>(u"editAddress"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        const auto* group = window->findChild<QQuickItem*>(u"editGroup"_s);
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* remove = window->findChild<QQuickItem*>(u"editRemove"_s);
        const auto* review = window->findChild<QQuickItem*>(u"editReview"_s);
        const auto* cancel = window->findChild<QQuickItem*>(u"editCancel"_s);
        const auto* save = window->findChild<QQuickItem*>(u"editSave"_s);
        const auto* no = window->findChild<QQuickItem*>(u"removeNo"_s);
        const auto* yes = window->findChild<QQuickItem*>(u"removeYes"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"removeWarning"_s);
        const auto* isPlain = window->findChild<QQuickItem*>(u"editPlain"_s);
        const auto* certificate = window->findChild<QQuickItem*>(u"editCertificate"_s);
        QVERIFY(name != nullptr && address != nullptr && password != nullptr && group != nullptr && lines != nullptr);
        QVERIFY(remove != nullptr && review != nullptr && cancel != nullptr && save != nullptr);
        QVERIFY(no != nullptr && yes != nullptr && warning != nullptr && isPlain != nullptr && certificate != nullptr);
        QCOMPARE(address->property("text").toString(), u"http://192.168.1.22:8080"_s);
        QCOMPARE(group->property("text").toString(), QString());
        QCOMPARE(group->property("placeholder").toString(), u"None"_s);
        QVERIFY(isPlain->isVisible());
        QCOMPARE(isPlain->property("text").toString(),
                 u"This KVM is spoken to over plain HTTP: its password, and what is seen and typed there, can be "
                 "read on the way."_s);
        QVERIFY(!certificate->isVisible());
        QVERIFY(!review->isVisible());
        QVERIFY(remove->isVisible() && cancel->isVisible() && save->isVisible());
        QVERIFY(!no->isVisible() && !yes->isVisible() && !warning->isVisible());
        keepPictureOf(window, u"edit-kvm-plain"_s);

        // Something is typed, and then "Remove": it is asked about first, in the box itself, in
        // the place of the lines. The keyboard is at the no.
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        type(window, u"Storage"_s);
        clickOn(window, remove);
        QTRY_VERIFY(says(window->contentItem(), u"Remove \"NAS\"?"_s));
        QVERIFY(!says(window->contentItem(), u"Edit \"NAS\""_s));
        QVERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(), u"The KVM disappears from the wall and its saved password is "
                                                       "erased. The KVM itself is not touched."_s);
        QVERIFY(!isSeen(name) && !isSeen(password) && !isSeen(isPlain));
        QVERIFY(no->isVisible() && yes->isVisible());
        QVERIFY(!remove->isVisible() && !cancel->isVisible() && !save->isVisible() && !review->isVisible());
        QCOMPARE(no->property("text").toString(), u"Cancel"_s);
        QCOMPARE(yes->property("text").toString(), u"Remove"_s);
        QVERIFY(yes->property("danger").toBool());
        QTRY_VERIFY(no->hasActiveFocus());
        QCOMPARE(removeAsked.count(), 0);
        keepPictureOf(window, u"edit-kvm-remove"_s);
        // Enter is no yes, and what is typed goes nowhere.
        QTest::keyClick(window, Qt::Key_Return);
        type(window, u"yes"_s);
        QCOMPARE(removeAsked.count(), 0);
        QCOMPARE(saveAsked.count(), 0);

        // The no leads back to the lines, as they were typed into.
        clickOn(window, no);
        QTRY_VERIFY(says(window->contentItem(), u"Edit \"NAS\""_s));
        QVERIFY(isSeen(name) && !warning->isVisible() && !yes->isVisible());
        QCOMPARE(name->property("text").toString(), u"Storage"_s);
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QCOMPARE(removeAsked.count(), 0);

        // The yes asks the program. Where it is refused, that is said under the question.
        clickOn(window, remove);
        QTRY_VERIFY(yes->isVisible());
        clickOn(window, yes);
        QCOMPARE(removeAsked.count(), 1);
        QCOMPARE(removeAsked[0][0].toString(), u"0a1b2c3d"_s);
        m_editing.began(u"removing"_s);
        m_editing.refused(u"Something else is being done to NAS at this moment. Wait for it to finish."_s);
        QTRY_VERIFY(says(window->contentItem(),
                         u"Something else is being done to NAS at this moment. Wait for it to finish."_s));
        QVERIFY(says(window->contentItem(), u"Remove \"NAS\"?"_s));
        QVERIFY(box->property("opened").toBool());
        keepPictureOf(window, u"edit-kvm-remove-refused"_s);
        // Taken off: the box closes. Nothing is told for it (the KVM is seen to be gone), and
        // nothing of it is kept.
        clickOn(window, yes);
        QCOMPARE(removeAsked.count(), 2);
        m_editing.began(u"removing"_s);
        m_editing.removed(u"NAS"_s);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(actions.notices()->count(), 0);
        QCOMPARE(name->property("text").toString(), QString());
        QCOMPARE(address->property("text").toString(), QString());
        QCOMPARE(m_editing.machine(), QString());
        QCOMPARE(m_editing.problem(), QString());

        // Opened again, for another KVM, it begins at its lines, with that one in them.
        qkvm::ui::EditKvmView::Shown other = aKvmOfTheWall();
        other.id = u"11223344"_s;
        other.called = u"192.168.1.30"_s;
        other.inASentence = u"192.168.1.30"_s;
        other.name.clear();
        other.address = u"192.168.1.30"_s;
        m_editing.show(other, false);
        QTRY_VERIFY(box->property("opened").toBool());
        // (One that has no name is called by where it is, in the title and in the empty line.)
        QVERIFY(says(window->contentItem(), u"Edit 192.168.1.30"_s));
        QVERIFY(isSeen(name) && !warning->isVisible());
        QCOMPARE(name->property("text").toString(), QString());
        QCOMPARE(name->property("placeholder").toString(), u"192.168.1.30"_s);
        QCOMPARE(address->property("text").toString(), u"192.168.1.30"_s);
        QVERIFY(review->isVisible());
        QVERIFY(certificate->isVisible());
        QVERIFY(!isPlain->isVisible());

        // The question, and then the box's own way out: nothing was asked, and nothing is kept.
        type(window, u"a name"_s);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        clickOn(window, remove);
        QTRY_VERIFY(yes->isVisible());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(removeAsked.count(), 2);
        QCOMPARE(saveAsked.count(), 0);
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(name->property("text").toString(), QString());
        QCOMPARE(m_editing.machine(), QString());
        // And "Cancel" under the lines closes it as well.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        QVERIFY(isSeen(name) && !yes->isVisible());
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"a password"_s);
        clickOn(window, cancel);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(m_editing.machine(), QString());
        QCOMPARE(saveAsked.count(), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theCertificateOfAKvmIsLookedAtInItsBoxAndSaidYesToWithAClickOnItsButton_data()
    {
        QTest::addColumn<bool>("pendingLayout");
        QTest::newRow("initial layout") << false;
        QTest::newRow("pending window layout") << true;
    }

    void theCertificateOfAKvmIsLookedAtInItsBoxAndSaidYesToWithAClickOnItsButton()
    {
        QFETCH(bool, pendingLayout);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy reviewAsked(&m_editing, &qkvm::ui::EditKvmView::reviewAsked);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QSignalSpy removeAsked(&m_editing, &qkvm::ui::EditKvmView::removeAsked);
        const QSignalSpy giveUpAsked(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked);
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* review = window->findChild<QQuickItem*>(u"editReview"_s);
        const auto* save = window->findChild<QQuickItem*>(u"editSave"_s);
        const auto* certificates = window->findChild<QQuickItem*>(u"certificates"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* match = window->findChild<QQuickItem*>(u"certificatesMatch"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(name != nullptr && review != nullptr && save != nullptr && certificates != nullptr);
        QVERIFY(remembered != nullptr && presented != nullptr && match != nullptr && warning != nullptr);
        QVERIFY(close != nullptr && trust != nullptr);
        QVERIFY(!certificates->isVisible() && !close->isVisible() && !trust->isVisible());
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };

        // Something is typed, and then "Review certificate": the program is asked, the button
        // says so meanwhile and keeps its place, and the lines stay until the two are there.
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        type(window, u"Storage"_s);
        if (pendingLayout) {
            window->resize(window->width() - 262, window->height() + 46);
        }
        // Both positions describe rendered layouts. clickOn() also waits for a
        // frame; comparing an unpolished baseline with that frame races resize.
        letTheWallComeToRest(window);
        const QRectF reviewWas = placeOf(review);
        clickOn(window, review);
        QCOMPARE(reviewAsked.count(), 1);
        QCOMPARE(reviewAsked[0][0].toString(), u"0a1b2c3d"_s);
        m_editing.began(u"looking"_s);
        QTRY_COMPARE(review->property("text").toString(), u"Checking…"_s);
        QCOMPARE(save->property("text").toString(), u"Save"_s);
        QVERIFY(!review->isEnabled() && !save->isEnabled());
        QVERIFY(isSeen(name) && !certificates->isVisible());
        letTheWallComeToRest(window);
        QCOMPARE(placeOf(review), reviewWas);

        // The same KVM as before: the two certificates in the place of the lines, each whole, in
        // lines of eight; that they match; and the way back, which is no yes to anything.
        m_editing.reviewed(oneCertificate(), oneCertificate());
        QTRY_VERIFY(certificates->isVisible());
        QVERIFY(says(window->contentItem(), u"\"NAS\" certificate"_s));
        QVERIFY(!says(window->contentItem(), u"Edit \"NAS\""_s));
        QVERIFY(!isSeen(name) && !review->isVisible() && !save->isVisible());
        QVERIFY(says(remembered, u"Remembered by qKVM:"_s));
        QVERIFY(says(remembered, oneInLines()));
        QVERIFY(says(presented, u"Presented by the device right now:"_s));
        QVERIFY(says(presented, oneInLines()));
        QVERIFY(match->isVisible());
        QCOMPARE(match->property("text").toString(), u"They match. qKVM is talking to the same KVM as before."_s);
        QVERIFY(!warning->isVisible());
        QVERIFY(close->isVisible());
        QCOMPARE(close->property("text").toString(), u"Close"_s);
        QVERIFY(!trust->isVisible());
        // Side by side, to be compared line by line; and not cut: every letter of each has its
        // room, inside its own half of the box.
        letTheWallComeToRest(window);
        QCOMPARE(placeOf(remembered).bottom(), placeOf(presented).bottom());
        QVERIFY(placeOf(remembered).right() < placeOf(presented).left());
        QCOMPARE(remembered->width(), presented->width());
        for (const QQuickItem* one : {remembered, presented}) {
            QList<const QQuickItem*> written;
            lines(one, written);
            written.removeIf([](const QQuickItem* line) { return !line->isVisible(); });
            QCOMPARE(written.size(), 2);
            for (const QQuickItem* line : std::as_const(written)) {
                QVERIFY(line->width() >= line->implicitWidth() - 0.5);
                QVERIFY(line->height() >= line->implicitHeight() - 0.5);
                QVERIFY(placeOf(one).adjusted(-0.5, -0.5, 0.5, 0.5).contains(placeOf(line)));
            }
        }
        keepPictureOf(window, u"edit-kvm-review-match"_s);
        clickOn(window, close);
        QTRY_VERIFY(says(window->contentItem(), u"Edit \"NAS\""_s));
        QVERIFY(isSeen(name) && !certificates->isVisible());
        // What was typed into the lines stands, and the two are forgotten.
        QCOMPARE(name->property("text").toString(), u"Storage"_s);
        QVERIFY(!m_editing.isReviewed());
        QCOMPARE(trustAsked.count(), 0);
        QVERIFY(box->property("opened").toBool());

        // It shows another one: both, the page's warning, and the yes, which does not stand out.
        clickOn(window, review);
        QCOMPARE(reviewAsked.count(), 2);
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(certificates->isVisible());
        QVERIFY(says(remembered, oneInLines()));
        QVERIFY(says(presented, anotherInLines()));
        QVERIFY(!match->isVisible());
        QVERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(),
                 u"A KVM gets a new certificate after a factory reset or some firmware updates. If neither happened, "
                 "something else may be answering at this address: do not trust it."_s);
        QVERIFY(close->isVisible() && trust->isVisible());
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        QCOMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(!trust->property("primary").toBool());
        // The yes is not in the row of the way back, where every other state of the box has its
        // buttons: it stands in the line of the title, above the two it is said to.
        letTheWallComeToRest(window);
        QVERIFY(placeOf(trust).bottom() < placeOf(certificates).top());
        QVERIFY(placeOf(certificates).bottom() < placeOf(close).top());
        QCOMPARE(trust->height(), close->height());
        keepPictureOf(window, u"edit-kvm-review-another"_s);

        // Enter, the space bar and what is typed do nothing here: a yes is a click on its button.
        // The keyboard is at neither button. (Tried once the button takes a click: a key that
        // did nothing before that would show nothing.)
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        QTRY_VERIFY(certificates->hasActiveFocus());
        QVERIFY(!trust->hasActiveFocus() && !close->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Enter);
        QTest::keyClick(window, Qt::Key_Space);
        type(window, u"yes"_s);
        QCOMPARE(trustAsked.count(), 0);
        QCOMPARE(saveAsked.count(), 0);
        QCOMPARE(removeAsked.count(), 0);
        QVERIFY(certificates->isVisible());
        QVERIFY(box->property("opened").toBool());

        // The click: the program is asked to have the KVM known by exactly what the box shows.
        const QRectF closeWas = placeOf(close);
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 1);
        QCOMPARE(trustAsked[0][0].toString(), u"0a1b2c3d"_s);
        QCOMPARE(trustAsked[0][1].toByteArray(), anotherCertificate());
        m_editing.began(u"trusting"_s);
        QTRY_COMPARE(trust->property("text").toString(), u"Checking…"_s);
        QVERIFY(!trust->isEnabled());
        QVERIFY(!box->property("canClose").toBool());
        letTheWallComeToRest(window);
        QCOMPARE(placeOf(close), closeWas);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(box->property("opened").toBool());
        // The KVM shows yet another by now: said, nothing is trusted, and the two stay as they
        // were shown. The keyboard is not left on the button that was clicked.
        m_editing.refused(u"The device now presents a different certificate than the one shown"_s);
        QTRY_VERIFY(
            says(window->contentItem(), u"The device now presents a different certificate than the one shown"_s));
        QVERIFY(certificates->isVisible());
        QVERIFY(says(presented, anotherInLines()));
        QCOMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QTRY_VERIFY(certificates->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Enter);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(trustAsked.count(), 1);
        keepPictureOf(window, u"edit-kvm-review-refused"_s);

        // A yes that is being asked is given up with "Cancel", and the two stay to be looked at.
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 2);
        m_editing.began(u"trusting"_s);
        QTRY_COMPARE(trust->property("text").toString(), u"Checking…"_s);
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        clickOn(window, close);
        QCOMPARE(giveUpAsked.count(), 1);
        m_editing.gaveUp();
        QTRY_COMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(certificates->isVisible());
        QVERIFY(box->property("opened").toBool());
        QTRY_VERIFY(certificates->hasActiveFocus());

        // Said yes to, and trusted: the box closes, and that is told as the page tells it.
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 3);
        QCOMPARE(trustAsked[2][1].toByteArray(), anotherCertificate());
        m_editing.began(u"trusting"_s);
        m_editing.trusted(u"NAS"_s);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(toldBy(actions), QStringList{u"ok: NAS: certificate trusted"_s});
        QCOMPARE(m_editing.presented(), QString());
        QVERIFY(!m_editing.isReviewed());
        QCOMPARE(name->property("text").toString(), QString());

        // A KVM that is known by no certificate yet (Windows vouched for it): that is said in the
        // place of one, and the one it shows can be said yes to.
        qkvm::ui::EditKvmView::Shown vouchedFor = aKvmOfTheWall();
        vouchedFor.certificate.clear();
        m_editing.show(vouchedFor, false);
        QTRY_VERIFY(box->property("opened").toBool());
        QVERIFY(says(window->contentItem(), u"No certificate remembered yet."_s));
        QVERIFY(isSeen(name) && !certificates->isVisible());
        clickOn(window, review);
        m_editing.began(u"looking"_s);
        m_editing.reviewed({}, oneCertificate());
        QTRY_VERIFY(certificates->isVisible());
        QVERIFY(says(remembered, u"none yet"_s));
        QVERIFY(says(presented, oneInLines()));
        QVERIFY(warning->isVisible() && trust->isVisible());
        keepPictureOf(window, u"edit-kvm-review-none-yet"_s);
        // "Cancel" leads back to the lines, and nothing was trusted.
        clickOn(window, close);
        QTRY_VERIFY(isSeen(name));
        QCOMPARE(trustAsked.count(), 3);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theButtonOnACardOpensTheBoxForItsKvm()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        // Two KVMs with a button on their cards: one that refused the sign-in, and one that shows
        // another certificate than it is known by.
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        const auto refused = std::ranges::find_if(
            tiles, [](const qkvm::ui::WallTile& tile) { return tile.action == u"Update login"_s; });
        QVERIFY(refused != tiles.end());
        const QString refusedKey = refused->key;
        const auto offline =
            std::ranges::find_if(tiles, [](const qkvm::ui::WallTile& tile) { return tile.state == u"offline"_s; });
        QVERIFY(offline != tiles.end());
        offline->state = u"error"_s;
        offline->label = u"Problem"_s;
        offline->title = u"Problem"_s;
        offline->text = u"Certificate does not match the one saved for this device"_s;
        offline->action = u"Review certificate"_s;
        const QString anotherKey = offline->key;
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy cardActionAsked(&actions, &qkvm::ui::ActionsView::cardActionAsked);
        const QSignalSpy openAsked(&m_editing, &qkvm::ui::EditKvmView::openAsked);
        const QSignalSpy reviewAsked(&m_editing, &qkvm::ui::EditKvmView::reviewAsked);
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        // Two cards have a button, and no other.
        QTRY_COMPARE(seenNamed(window, u"cardAction"_s), 2);
        keepPictureOf(window, u"own-wall-card-buttons"_s);

        // "Update login": the program is told whose card it was.
        const QQuickItem* update = inCard(cardOf(window->contentItem(), refusedKey), u"cardAction"_s);
        QVERIFY(update != nullptr);
        QCOMPARE(update->property("text").toString(), u"Update login"_s);
        clickOn(window, update);
        QCOMPARE(cardActionAsked.count(), 1);
        QCOMPARE(cardActionAsked[0][0].toString(), refusedKey);
        // (A click on the button is not a click on the card: the machine is not shown large.)
        QCOMPARE(wall.enlarged(), QString());
        // The program asks for the box, the window passes that to its own box's view, and the
        // program opens the box at its lines.
        emit actions.editAsked(refusedKey, false);
        QCOMPARE(openAsked.count(), 1);
        QCOMPARE(openAsked[0][0].toString(), refusedKey);
        QCOMPARE(openAsked[0][1].toBool(), false);
        qkvm::ui::EditKvmView::Shown kvm = aKvmOfTheWall();
        kvm.id = refusedKey;
        kvm.called = u"Backup box"_s;
        kvm.inASentence = u"\"Backup box\""_s;
        kvm.name = u"Backup box"_s;
        m_editing.show(kvm, false);
        QTRY_VERIFY(box->property("opened").toBool());
        QVERIFY(says(window->contentItem(), u"Edit \"Backup box\""_s));
        QCOMPARE(reviewAsked.count(), 0);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // "Review certificate": the box is opened for the certificate alone. It shows the one the
        // KVM is known by at once, and that the one it shows now is being read.
        const QQuickItem* look = inCard(cardOf(window->contentItem(), anotherKey), u"cardAction"_s);
        QVERIFY(look != nullptr);
        QCOMPARE(look->property("text").toString(), u"Review certificate"_s);
        clickOn(window, look);
        QCOMPARE(cardActionAsked.count(), 2);
        QCOMPARE(cardActionAsked[1][0].toString(), anotherKey);
        emit actions.editAsked(anotherKey, true);
        QCOMPARE(openAsked.count(), 2);
        QCOMPARE(openAsked[1][0].toString(), anotherKey);
        QCOMPARE(openAsked[1][1].toBool(), true);
        kvm.id = anotherKey;
        kvm.called = u"Old server"_s;
        kvm.inASentence = u"\"Old server\""_s;
        kvm.name = u"Old server"_s;
        m_editing.show(kvm, true);
        QTRY_VERIFY(box->property("opened").toBool());
        QCOMPARE(reviewAsked.count(), 1);
        QCOMPARE(reviewAsked[0][0].toString(), anotherKey);
        m_editing.began(u"looking"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* certificates = window->findChild<QQuickItem*>(u"certificates"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        const auto* remove = window->findChild<QQuickItem*>(u"editRemove"_s);
        QVERIFY(name != nullptr && certificates != nullptr && remembered != nullptr && presented != nullptr);
        QVERIFY(close != nullptr && trust != nullptr && remove != nullptr);
        QVERIFY(says(window->contentItem(), u"\"Old server\" certificate"_s));
        QTRY_VERIFY(certificates->isVisible());
        QVERIFY(!isSeen(name) && !remove->isVisible());
        QVERIFY(says(remembered, oneInLines()));
        QTRY_VERIFY(says(presented, u"Checking…"_s));
        QVERIFY(!trust->isVisible());
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        keepPictureOf(window, u"edit-kvm-review-reading"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(says(presented, anotherInLines()));
        QVERIFY(trust->isVisible());
        QTRY_VERIFY(certificates->hasActiveFocus());
        // Its "Cancel" leaves the box: there are no lines to go back to. Nothing was trusted.
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        clickOn(window, close);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(trustAsked.count(), 0);
        QCOMPARE(m_editing.machine(), QString());

        // Where the certificate could not be read, that is said in the same place, and the box
        // has only its way out.
        m_editing.show(kvm, true);
        QTRY_VERIFY(box->property("opened").toBool());
        QCOMPARE(reviewAsked.count(), 2);
        m_editing.began(u"looking"_s);
        m_editing.refused(u"Device is unreachable"_s);
        const auto* problem = window->findChild<QQuickItem*>(u"editProblem"_s);
        QVERIFY(problem != nullptr);
        QTRY_VERIFY(isSeen(problem));
        QCOMPARE(problem->property("text").toString(), u"Device is unreachable"_s);
        QVERIFY(certificates->isVisible());
        QVERIFY(!presented->isVisible() && !trust->isVisible() && !isSeen(name));
        QCOMPARE(close->property("text").toString(), u"Close"_s);
        keepPictureOf(window, u"edit-kvm-review-unreachable"_s);
        clickOn(window, close);
        QTRY_VERIFY(!box->property("opened").toBool());

        // A machine that is shown large has the same button, and it leads the same way.
        wall.enlarge(refusedKey);
        QTRY_VERIFY(window->findChild<QQuickItem*>(u"enlargedScreen"_s) != nullptr);
        const QQuickItem* large = inCard(window->findChild<QQuickItem*>(u"enlargedScreen"_s), u"cardAction"_s);
        QVERIFY(large != nullptr);
        QTRY_VERIFY(isSeen(large));
        clickOn(window, large);
        QCOMPARE(cardActionAsked.count(), 3);
        QCOMPARE(cardActionAsked[2][0].toString(), refusedKey);
        wall.closeEnlarged();

        // At a server's wall nobody is led to this box, whatever asks.
        session.show(signedIn());
        emit actions.editAsked(refusedKey, false);
        emit actions.editAsked(anotherKey, true);
        QCOMPARE(openAsked.count(), 2);
        QVERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxThatChangesAKvmFitsTheSmallestWindow()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->resize(window->minimumWidth(), window->minimumHeight());
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        // With the longest it ever says: a complaint of several lines.
        const QString complaint =
            u"This device is on the wall over HTTPS. It is not changed to plain HTTP at the same address, where its "
            "password would travel unencrypted. To use it that way, remove it from the wall and add it again."_s;
        m_editing.began(u"saving"_s);
        m_editing.refused(complaint);
        QTRY_VERIFY(says(window->contentItem(), complaint));
        letTheWallComeToRest(window);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const auto isInSight = [&](const QString& name) {
            const auto* item = window->findChild<QQuickItem*>(name);
            return item != nullptr && isSeen(item) && inside.contains(placeOf(item));
        };
        // What came of it and the buttons are in sight whatever the window's size.
        for (const QString& name :
             {u"editProblem"_s, u"editRemove"_s, u"editReview"_s, u"editCancel"_s, u"editSave"_s, u"editLines"_s}) {
            QVERIFY2(isInSight(name), qPrintable(name));
        }
        // The lines to type into are scrolled where there is no room for all, and a mark says so:
        // each is in sight when the keyboard comes to it, forwards and back.
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* mark = window->findChild<QQuickItem*>(u"editScrollMark"_s);
        QVERIFY(lines->property("contentHeight").toReal() > lines->height());
        QVERIFY(mark != nullptr && mark->isVisible());
        // (Beside the lines, in the box's margin: over none of them.)
        QVERIFY(placeOf(mark).left() >= placeOf(lines).right());
        QVERIFY(inside.contains(placeOf(mark)));
        const QStringList inTurn{u"editName"_s, u"editAddress"_s, u"editUser"_s, u"editPassword"_s, u"editGroup"_s};
        for (int at = 0; at < inTurn.size() * 2 - 1; ++at) {
            const QString& name = inTurn.at(at < inTurn.size() ? at : inTurn.size() * 2 - 2 - at);
            const auto* line = window->findChild<QQuickItem*>(name);
            QVERIFY2(line != nullptr, qPrintable(name));
            QTRY_VERIFY2(line->property("hasKeyboard").toBool(), qPrintable(name));
            QTRY_VERIFY2(placeOf(lines).adjusted(0, -1, 0, 1).contains(placeOf(line)), qPrintable(name));
            QTest::keyClick(window, at < inTurn.size() - 1 ? Qt::Key_Tab : Qt::Key_Backtab);
        }
        keepPictureOf(window, u"edit-kvm-smallest"_s);

        // The question before the KVM is taken off: all of it, and its two buttons. What was
        // complained of under the lines is not said under the question: it is not about that.
        clickOn(window, window->findChild<QQuickItem*>(u"editRemove"_s));
        QTRY_VERIFY(isInSight(u"removeWarning"_s));
        for (const QString& name : {u"removeNo"_s, u"removeYes"_s}) {
            QVERIFY2(isInSight(name), qPrintable(name));
        }
        QVERIFY(!says(window->contentItem(), complaint));
        letTheWallComeToRest(window);
        QVERIFY(!mark->isVisible());
        keepPictureOf(window, u"edit-kvm-remove-smallest"_s);
        clickOn(window, window->findChild<QQuickItem*>(u"removeNo"_s));
        QTRY_VERIFY(isInSight(u"editSave"_s));
        QVERIFY(says(window->contentItem(), complaint));

        // And the two certificates, with the warning and a complaint under them: all of it is in
        // sight at once, so that nobody says yes with the warning out of sight.
        clickOn(window, window->findChild<QQuickItem*>(u"editReview"_s));
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isInSight(u"reviewTrust"_s));
        QVERIFY(!says(window->contentItem(), complaint));
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QQuickItem*>(u"reviewTrust"_s)->isEnabled(), 5000);
        clickOn(window, window->findChild<QQuickItem*>(u"reviewTrust"_s));
        m_editing.began(u"trusting"_s);
        m_editing.refused(u"The device now presents a different certificate than the one shown"_s);
        QTRY_VERIFY(
            says(window->contentItem(), u"The device now presents a different certificate than the one shown"_s));
        letTheWallComeToRest(window);
        for (const QString& name : {u"certificateRemembered"_s, u"certificatePresented"_s, u"certificateWarning"_s,
                                    u"editProblem"_s, u"reviewClose"_s, u"reviewTrust"_s, u"editLines"_s}) {
            QVERIFY2(isInSight(name), qPrintable(name));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        QVERIFY(!mark->isVisible());
        keepPictureOf(window, u"edit-kvm-review-smallest"_s);

        // So with the two of a saving that was refused over what is shown at the address that
        // was typed, where what stands over the second takes a line more.
        clickOn(window, window->findChild<QQuickItem*>(u"reviewClose"_s));
        QTRY_VERIFY(isInSight(u"editSave"_s));
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30:8443"_s);
        QTRY_VERIFY(isInSight(u"reviewTrust"_s));
        letTheWallComeToRest(window);
        for (const QString& name : {u"certificateRemembered"_s, u"certificatePresented"_s, u"certificateWarning"_s,
                                    u"reviewClose"_s, u"reviewTrust"_s, u"editLines"_s}) {
            QVERIFY2(isInSight(name), qPrintable(name));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        QVERIFY(!mark->isVisible());
        keepPictureOf(window, u"edit-kvm-another-at-a-new-address-smallest"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxThatChangesAKvmGoesWithThisPcsOwnWall()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        QVERIFY(box != nullptr && name != nullptr && password != nullptr);
        const QSignalSpy giveUpAsked(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked);

        // Another window went on to a server, and this one shows that server's wall now: the box
        // is not left open over it, with a password typed into it and a KVM of another wall in it.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        session.show(signedIn());
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(name->property("text").toString(), QString());
        QCOMPARE(m_editing.machine(), QString());
        QCOMPARE(giveUpAsked.count(), 0);

        // Nor over the screen that asks for a server; and what was being asked is given up.
        session.show(atItsOwnWall());
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        session.show({});
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(giveUpAsked.count(), 1);
        QVERIFY(!m_editing.busy());
        QCOMPARE(m_editing.machine(), QString());

        // So with the two certificates on show, and a yes to one of them on its way.
        session.show(atItsOwnWall());
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        m_editing.trust();
        m_editing.began(u"trusting"_s);
        session.show(signedIn());
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(giveUpAsked.count(), 2);
        QVERIFY(!m_editing.isReviewed());
        QCOMPARE(m_editing.presented(), QString());
        // What comes for it afterwards is nobody's: nothing is told, and the box stays closed.
        m_editing.trusted(u"NAS"_s);
        QTest::qWait(100);
        QVERIFY(!box->property("opened").toBool());
        QCOMPARE(m_noActions.back()->notices()->count(), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatIsShownAtAnAddressThatWasTypedIsLookedAtInTheBoxBeforeThePasswordGoesThere()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const QSignalSpy giveUpAsked(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked);
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* address = window->findChild<QQuickItem*>(u"editAddress"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        const auto* cancel = window->findChild<QQuickItem*>(u"editCancel"_s);
        const auto* save = window->findChild<QQuickItem*>(u"editSave"_s);
        const auto* certificates = window->findChild<QQuickItem*>(u"certificates"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* match = window->findChild<QQuickItem*>(u"certificatesMatch"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        const auto* problem = window->findChild<QQuickItem*>(u"editProblem"_s);
        const auto* saved = window->findChild<QQuickItem*>(u"editSaved"_s);
        const auto* savedCertificate = window->findChild<QQuickItem*>(u"editSavedCertificate"_s);
        QVERIFY(name != nullptr && address != nullptr && password != nullptr && cancel != nullptr && save != nullptr);
        QVERIFY(certificates != nullptr && remembered != nullptr && presented != nullptr && match != nullptr);
        QVERIFY(warning != nullptr && close != nullptr && trust != nullptr && problem != nullptr);
        QVERIFY(saved != nullptr && savedCertificate != nullptr);
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };

        // Another address is typed over the one that stands there, and its password with it.
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(address->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        type(window, u"192.168.1.30"_s);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(password->property("hasKeyboard").toBool());
        type(window, u"secret"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked[0][2].toString(), u"192.168.1.30"_s);
        QCOMPARE(saveAsked[0][4].toString(), u"secret"_s);
        QCOMPARE(saveAsked[0][6].toByteArray(), QByteArray());
        m_editing.began(u"saving"_s);
        QTRY_COMPARE(save->property("text").toString(), u"Checking…"_s);
        QVERIFY(!trust->isVisible());

        // What answers there shows another certificate than the KVM is known by: the two in the
        // place of the lines, as a look at its certificate shows them, the second under what
        // says where it was shown; the page's warning; the way back; and the yes.
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(certificates->isVisible());
        QVERIFY(says(window->contentItem(), u"\"NAS\" certificate"_s));
        QVERIFY(!says(window->contentItem(), u"Edit \"NAS\""_s));
        QVERIFY(!isSeen(name) && !isSeen(password) && !save->isVisible() && !cancel->isVisible());
        QVERIFY(says(remembered, u"Remembered by qKVM:"_s));
        QVERIFY(says(remembered, oneInLines()));
        QVERIFY(says(presented, u"Presented right now at the new address, 192.168.1.30:"_s));
        QVERIFY(says(presented, anotherInLines()));
        QVERIFY(!says(presented, u"Presented by the device right now:"_s));
        QVERIFY(!match->isVisible());
        QVERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(),
                 u"A KVM gets a new certificate after a factory reset or some firmware updates. If neither happened, "
                 "something else may be answering at this address: do not trust it."_s);
        QVERIFY(!isSeen(problem));
        QVERIFY(close->isVisible() && trust->isVisible());
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        QCOMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(!trust->property("primary").toBool());
        // Side by side, each whole in its own half, though what stands over the second is longer.
        letTheWallComeToRest(window);
        QCOMPARE(placeOf(remembered).bottom(), placeOf(presented).bottom());
        QVERIFY(placeOf(remembered).right() < placeOf(presented).left());
        for (const QQuickItem* one : {remembered, presented}) {
            QList<const QQuickItem*> written;
            lines(one, written);
            written.removeIf([](const QQuickItem* line) { return !line->isVisible(); });
            QCOMPARE(written.size(), 2);
            for (const QQuickItem* line : std::as_const(written)) {
                // (What says where it was shown takes a second line, and is whole on the two.)
                QVERIFY(line->property("contentWidth").toReal() <= line->width() + 0.5);
                QVERIFY(line->height() >= line->implicitHeight() - 0.5);
                QVERIFY(placeOf(one).adjusted(-0.5, -0.5, 0.5, 0.5).contains(placeOf(line)));
            }
        }
        keepPictureOf(window, u"edit-kvm-another-at-a-new-address"_s);
        // The keyboard is at neither button, and no key is a yes.
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        QTRY_VERIFY(certificates->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Enter);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(trustAsked.count(), 0);

        // The way back: the lines as they were typed into, the password too. Nothing was asked.
        clickOn(window, close);
        QTRY_VERIFY(isSeen(name));
        QVERIFY(says(window->contentItem(), u"Edit \"NAS\""_s));
        QVERIFY(!certificates->isVisible() && !trust->isVisible());
        QCOMPARE(address->property("text").toString(), u"192.168.1.30"_s);
        QCOMPARE(password->property("text").toString(), u"secret"_s);
        QCOMPARE(saveAsked.count(), 1);
        QVERIFY(!m_editing.differs());
        QVERIFY(box->property("opened").toBool());

        // Saved again, it is shown again; and the yes is a click on its button, once it takes
        // one. It asks for the saving of what stands in the lines, and the program hands the
        // certificate that is shown on with it.
        clickOn(window, save);
        QCOMPARE(saveAsked.count(), 2);
        QCOMPARE(saveAsked[1][6].toByteArray(), QByteArray());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(isSeen(trust));
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 3);
        QCOMPARE(saveAsked[2][0].toString(), u"0a1b2c3d"_s);
        QCOMPARE(saveAsked[2][1].toString(), u"NAS"_s);
        QCOMPARE(saveAsked[2][2].toString(), u"192.168.1.30"_s);
        QCOMPARE(saveAsked[2][3].toString(), u"admin"_s);
        QCOMPARE(saveAsked[2][4].toString(), u"secret"_s);
        QCOMPARE(saveAsked[2][5].toString(), u"Rack A"_s);
        QCOMPARE(saveAsked[2][6].toByteArray(), anotherCertificate());
        QCOMPARE(trustAsked.count(), 0);
        // While it is asked: said at the button, and "Cancel" gives it up; the two stay.
        m_editing.began(u"saving"_s);
        QTRY_COMPARE(trust->property("text").toString(), u"Checking…"_s);
        QVERIFY(!trust->isEnabled());
        QVERIFY(!box->property("canClose").toBool());
        clickOn(window, close);
        QCOMPARE(giveUpAsked.count(), 1);
        QCOMPARE(giveUpAsked[0][0].toBool(), false);
        m_editing.gaveUp();
        QTRY_COMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(certificates->isVisible());

        // A yes that is refused (the password is not the KVM's): said at the lines, where it is
        // put right. What was typed stands, and the two are shown no longer.
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 4);
        m_editing.began(u"saving"_s);
        m_editing.refused(u"Login rejected - check the username and password"_s);
        QTRY_VERIFY(isSeen(name));
        QVERIFY(!certificates->isVisible() && !trust->isVisible());
        QTRY_VERIFY(isSeen(problem));
        QCOMPARE(problem->property("text").toString(), u"Login rejected - check the username and password"_s);
        QCOMPARE(password->property("text").toString(), u"secret"_s);
        QCOMPARE(address->property("text").toString(), u"192.168.1.30"_s);

        // And one that is taken: the box does not close. It says that the KVM was saved, and
        // shows what it is known by from now on, whole; its lines are the KVM as it is now, and
        // the password that was typed is gone.
        clickOn(window, save);
        QCOMPARE(saveAsked.count(), 5);
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(isSeen(trust));
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 6);
        QCOMPARE(saveAsked[5][6].toByteArray(), anotherCertificate());
        m_editing.began(u"saving"_s);
        qkvm::ui::EditKvmView::Shown now = movedKvm(false);
        now.address = u"192.168.1.30"_s;
        m_editing.savedAndStays(now);
        QTRY_VERIFY(isSeen(saved));
        QVERIFY(box->property("opened").toBool());
        QVERIFY(isSeen(name) && !certificates->isVisible() && !trust->isVisible() && !isSeen(problem));
        QCOMPARE(saved->property("text").toString(), u"\"NAS\" saved"_s);
        QVERIFY(isSeen(savedCertificate));
        QCOMPARE(savedCertificate->property("text").toString(), u"Remembered SHA-256: "_s + writtenAnother());
        QVERIFY(savedCertificate->height() >= savedCertificate->implicitHeight() - 0.5);
        QCOMPARE(address->property("text").toString(), u"192.168.1.30"_s);
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(cancel->property("text").toString(), u"Done"_s);
        QCOMPARE(actions.notices()->count(), 0);
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        keepPictureOf(window, u"edit-kvm-saved-known-by-another"_s);
        // Its way out closes it, and nothing of it is kept.
        clickOn(window, cancel);
        QTRY_VERIFY(!box->property("opened").toBool());
        QCOMPARE(m_editing.savedAs(), QString());
        QCOMPARE(m_editing.machine(), QString());
        QCOMPARE(giveUpAsked.count(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatASavingChangedOfHowAKvmIsKnownOrSpokenToIsInSightInTheBoxThatStays_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void whatASavingChangedOfHowAKvmIsKnownOrSpokenToIsInSightInTheBoxThatStays()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* address = window->findChild<QQuickItem*>(u"editAddress"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        const auto* group = window->findChild<QQuickItem*>(u"editGroup"_s);
        const auto* saved = window->findChild<QQuickItem*>(u"editSaved"_s);
        const auto* savedPlain = window->findChild<QQuickItem*>(u"editSavedPlain"_s);
        const auto* savedCertificate = window->findChild<QQuickItem*>(u"editSavedCertificate"_s);
        const auto* problem = window->findChild<QQuickItem*>(u"editProblem"_s);
        const auto* cancel = window->findChild<QQuickItem*>(u"editCancel"_s);
        QVERIFY(name != nullptr && address != nullptr && password != nullptr && group != nullptr);
        QVERIFY(saved != nullptr && savedPlain != nullptr && savedCertificate != nullptr && problem != nullptr);
        QVERIFY(cancel != nullptr);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto isInSight = [&](const QQuickItem* item) {
            return isSeen(item) && inside.contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())));
        };
        const QString sizes = u"-%1x%2"_s.arg(size.width()).arg(size.height());
        QVERIFY(!isSeen(saved) && !isSeen(savedPlain) && !isSeen(savedCertificate));

        // Saved at another address, where it is known by another certificate: the box stays, and
        // says so under its lines, where it is in sight whatever the window's size. The
        // certificate is there whole, to be compared with what the KVM says of itself.
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"secret"_s);
        m_editing.began(u"saving"_s);
        m_editing.savedAndStays(movedKvm(false));
        QTRY_VERIFY(isSeen(saved));
        letTheWallComeToRest(window);
        QVERIFY(isInSight(saved) && isInSight(savedCertificate) && isInSight(cancel));
        QVERIFY(!isSeen(savedPlain) && !isSeen(problem));
        QCOMPARE(saved->property("text").toString(), u"\"NAS\" saved"_s);
        QCOMPARE(savedCertificate->property("text").toString(), u"Remembered SHA-256: "_s + writtenAnother());
        QVERIFY(savedCertificate->height() >= savedCertificate->implicitHeight() - 0.5);
        QVERIFY(savedCertificate->width() >= savedCertificate->property("contentWidth").toReal() - 0.5);
        QCOMPARE(cancel->property("text").toString(), u"Done"_s);
        // The lines are the KVM as it is now, and no password.
        QCOMPARE(address->property("text").toString(), u"192.168.1.30:8443"_s);
        QCOMPARE(password->property("text").toString(), QString());
        QVERIFY(box->property("opened").toBool());
        keepPictureOf(window, u"edit-kvm-saved-known-by-another"_s + sizes);

        // The next thing that is asked says nothing of the last. Saved to where it is spoken to
        // in the clear: that is said as what it is, and no certificate is.
        m_editing.began(u"saving"_s);
        QTRY_VERIFY(!isSeen(saved));
        QCOMPARE(cancel->property("text").toString(), u"Cancel"_s);
        m_editing.savedAndStays(movedKvm(true));
        QTRY_VERIFY(isSeen(savedPlain));
        letTheWallComeToRest(window);
        QVERIFY(isInSight(saved) && isInSight(savedPlain) && isInSight(cancel));
        QVERIFY(!isSeen(savedCertificate));
        QCOMPARE(savedPlain->property("text").toString(),
                 u"This KVM is spoken to over plain HTTP: its password, and what is seen and typed there, can be "
                 "read on the way."_s);
        QVERIFY(savedPlain->property("text").toString().contains(u"over plain HTTP"_s));
        QCOMPARE(address->property("text").toString(), u"http://192.168.1.30:8080"_s);
        keepPictureOf(window, u"edit-kvm-saved-in-the-clear"_s + sizes);

        // And one that Windows vouches for from now on is known by no certificate: said so.
        qkvm::ui::EditKvmView::Shown vouchedFor = movedKvm(false);
        vouchedFor.certificate.clear();
        m_editing.began(u"saving"_s);
        m_editing.savedAndStays(vouchedFor);
        QTRY_VERIFY(isSeen(savedCertificate));
        QCOMPARE(savedCertificate->property("text").toString(), u"No certificate remembered yet."_s);
        // And a JetKVM that answers over HTTPS now, at its own host, which it was saved over: said so,
        // above the certificate it is known by from here on; and of nothing else.
        const auto* savedOverHttps = window->findChild<QQuickItem*>(u"editSavedOverHttps"_s);
        QVERIFY(savedOverHttps != nullptr);
        QVERIFY(!isSeen(savedOverHttps));
        m_editing.began(u"saving"_s);
        m_editing.savedAndStays(movedKvm(false), true);
        QTRY_VERIFY(isSeen(savedOverHttps));
        letTheWallComeToRest(window);
        QCOMPARE(savedOverHttps->property("text").toString(), u"It answers over HTTPS now."_s);
        QVERIFY(isInSight(savedOverHttps) && isInSight(savedCertificate));
        QVERIFY(savedOverHttps->mapToScene(QPointF(0, 0)).y() < savedCertificate->mapToScene(QPointF(0, 0)).y());
        m_editing.began(u"saving"_s);
        m_editing.savedAndStays(vouchedFor);
        QTRY_VERIFY(isSeen(savedCertificate));
        QVERIFY(!isSeen(savedOverHttps));
        // Where the address that was asked is another than the one typed (a JetKVM asked both ways), the line
        // says that one from here on.
        m_editing.retype(u"http://192.168.1.31"_s);
        QTRY_COMPARE(address->property("text").toString(), u"http://192.168.1.31"_s);

        // A KVM that another window changed meanwhile: why it was not saved is said, the lines
        // are filled anew with it as it is, and what was typed is gone, the password too.
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        type(window, u"Typed"_s);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"secret"_s);
        QCOMPARE(name->property("text").toString(), u"Typed"_s);
        QCOMPARE(password->property("text").toString(), u"secret"_s);
        const QString changedMeanwhile =
            u"This device was changed meanwhile. Look at it again. The fields were filled in again with what is saved "
            "now."_s;
        qkvm::ui::EditKvmView::Shown renamed = aKvmOfTheWall();
        renamed.called = u"Storage"_s;
        renamed.inASentence = u"\"Storage\""_s;
        renamed.name = u"Storage"_s;
        renamed.group = u"Rack B"_s;
        m_editing.began(u"saving"_s);
        m_editing.refusedOver(renamed, changedMeanwhile);
        QTRY_VERIFY(isSeen(problem));
        letTheWallComeToRest(window);
        QVERIFY(isInSight(problem) && isInSight(cancel));
        QCOMPARE(problem->property("text").toString(), changedMeanwhile);
        QVERIFY(!isSeen(saved));
        QVERIFY(says(window->contentItem(), u"Edit \"Storage\""_s));
        QCOMPARE(name->property("text").toString(), u"Storage"_s);
        QCOMPARE(address->property("text").toString(), u"192.168.1.22"_s);
        QCOMPARE(group->property("text").toString(), u"Rack B"_s);
        QCOMPARE(password->property("text").toString(), QString());
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        keepPictureOf(window, u"edit-kvm-changed-meanwhile"_s + sizes);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aBoxIsClosedFromOutsideWithNothingKeptOfWhatWasTyped()
    {
        // Whoever keeps the windows closes the boxes of a window that is out of sight (behind
        // the program's icon): what was typed into them is emptied, the password least of all
        // left in a line nobody sees.
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_editing.closed();
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* editBox = window->findChild<QObject*>(u"editKvm"_s);
        QObject* addBox = window->findChild<QObject*>(u"addKvm"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        const auto* newAddress = window->findChild<QQuickItem*>(u"kvmAddress"_s);
        const auto* newPassword = window->findChild<QQuickItem*>(u"kvmPassword"_s);
        QVERIFY(editBox != nullptr && addBox != nullptr && name != nullptr && password != nullptr);
        QVERIFY(newAddress != nullptr && newPassword != nullptr);
        const QSignalSpy giveUpAsked(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked);
        const QSignalSpy givingUpAnAdding(&m_adding, &qkvm::ui::AddKvmView::giveUpAsked);

        // The box that changes a KVM, with a password typed into it.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(editBox->property("opened").toBool());
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        m_editing.close();
        QTRY_VERIFY(!editBox->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(name->property("text").toString(), QString());
        QCOMPARE(m_editing.machine(), QString());
        QCOMPARE(giveUpAsked.count(), 0);
        // And with a saving under way, and the two certificates of one that was refused: given
        // up, by a box that hears nothing of it any more.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(editBox->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
        m_editing.began(u"saving"_s);
        m_editing.close();
        QTRY_VERIFY(!editBox->property("opened").toBool());
        QCOMPARE(giveUpAsked.count(), 1);
        QCOMPARE(giveUpAsked[0][0].toBool(), true);
        QVERIFY(!m_editing.busy());
        QVERIFY(!m_editing.differs());
        QCOMPARE(m_editing.presented(), QString());

        // The box that adds one, with an address and a password typed into it.
        QVERIFY(QMetaObject::invokeMethod(addBox, "open"));
        QTRY_VERIFY(addBox->property("opened").toBool());
        type(window, u"192.168.1.20"_s);
        QTest::keyClick(window, Qt::Key_Return);
        type(window, u"a password"_s);
        QCOMPARE(newPassword->property("text").toString(), u"a password"_s);
        m_adding.close();
        QTRY_VERIFY(!addBox->property("opened").toBool());
        QCOMPARE(newPassword->property("text").toString(), QString());
        QCOMPARE(newAddress->property("text").toString(), QString());
        QCOMPARE(givingUpAnAdding.count(), 0);
        // And with a KVM that is being asked: given up.
        QVERIFY(QMetaObject::invokeMethod(addBox, "open"));
        QTRY_VERIFY(addBox->property("opened").toBool());
        m_adding.began();
        m_adding.close();
        QTRY_VERIFY(!addBox->property("opened").toBool());
        QVERIFY(givingUpAnAdding.count() >= 1);
        m_adding.gaveUp();
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theYesToACertificateStandsWhereNoOtherButtonOfTheBoxEverStands_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void theYesToACertificateStandsWhereNoOtherButtonOfTheBoxEverStands()
    {
        // The two certificates arrive when whatever answers at the KVM's address has answered: a
        // click that was on its way to the button that stood there a moment before is no yes.
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QCOMPARE(window->size(), size);
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        QVERIFY(box != nullptr);
        const QQuickItem* all = box->property("contentItem").value<QQuickItem*>()->parentItem();
        QVERIFY(all != nullptr);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* certificates = window->findChild<QQuickItem*>(u"certificates"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(name != nullptr && certificates != nullptr && trust != nullptr);

        // Where every button of the box stood in each of its states, the yes apart from the rest.
        // (And a picture of each state, for whoever wants to look at them.)
        QList<Stood> yes;
        QList<Stood> others;
        const auto look = [&](const QString& state) {
            letTheWallComeToRest(window);
            QList<Stood> seen;
            buttonsUnder(all, state, seen);
            for (const Stood& one : std::as_const(seen)) {
                (one.button == u"reviewTrust"_s ? yes : others).append(one);
            }
            QString file = state;
            keepPictureOf(window, u"edit-kvm-%1x%2-"_s.arg(size.width()).arg(size.height()) +
                                      file.replace(u", "_s, u"-"_s).replace(u' ', u'-'));
        };
        const QString complaint =
            u"This device is on the wall over HTTPS. It is not changed to plain HTTP at the same address, where its "
            "password would travel unencrypted. To use it that way, remove it from the wall and add it again."_s;
        const QString shownAnother = u"The device now presents a different certificate than the one shown"_s;

        // A box that was opened from the lines: the lines, the question before the KVM is taken
        // off, and the two certificates, each while something is asked and after it was refused.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_VERIFY(isSeen(name));
        look(u"the lines"_s);
        m_editing.began(u"saving"_s);
        look(u"the lines, saving"_s);
        m_editing.refused(complaint);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        look(u"the lines, refused"_s);
        clickOn(window, window->findChild<QQuickItem*>(u"editRemove"_s));
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"removeYes"_s)));
        look(u"the question"_s);
        m_editing.began(u"removing"_s);
        m_editing.refused(u"Something else is being done to NAS at this moment. Wait for it to finish."_s);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        look(u"the question, refused"_s);
        clickOn(window, window->findChild<QQuickItem*>(u"removeNo"_s));
        QTRY_VERIFY(isSeen(name));
        m_editing.began(u"looking"_s);
        look(u"the lines, looking"_s);
        m_editing.reviewed(oneCertificate(), oneCertificate());
        QTRY_VERIFY(isSeen(certificates));
        look(u"two that match"_s);
        clickOn(window, window->findChild<QQuickItem*>(u"reviewClose"_s));
        QTRY_VERIFY(isSeen(name));
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        look(u"two that differ"_s);
        m_editing.began(u"trusting"_s);
        look(u"two that differ, trusting"_s);
        m_editing.refused(shownAnother);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        look(u"two that differ, refused"_s);
        // A saving that is refused over what is shown at the address that was typed, the yes
        // to that, and the box that stays open on what was saved; and on a KVM that another
        // window changed meanwhile.
        clickOn(window, window->findChild<QQuickItem*>(u"reviewClose"_s));
        QTRY_VERIFY(isSeen(name));
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30:8443"_s);
        QTRY_VERIFY(isSeen(trust));
        look(u"another at a new address"_s);
        QVERIFY(!trust->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        look(u"another at a new address, after a moment"_s);
        m_editing.began(u"saving"_s);
        look(u"another at a new address, saving"_s);
        m_editing.savedAndStays(movedKvm(false));
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editSaved"_s)));
        look(u"saved, known by another"_s);
        m_editing.began(u"saving"_s);
        m_editing.savedAndStays(movedKvm(true));
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editSavedPlain"_s)));
        look(u"saved, in the clear"_s);
        m_editing.began(u"saving"_s);
        m_editing.refusedOver(aKvmOfTheWall(), u"This device was changed meanwhile. Look at it again. The fields were "
                                               "filled in again with what is saved now."_s);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        look(u"changed meanwhile"_s);
        // What is shown at the address that was typed has taken the place of what was said yes
        // to; and Windows vouches there, so that it is its word a yes is given to.
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30:8443"_s);
        QTRY_VERIFY_WITH_TIMEOUT(isSeen(trust) && trust->isEnabled(), 5000);
        clickOn(window, trust);
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), QByteArray(32, 'c'), u"192.168.1.30:8443"_s);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"certificateAnotherNow"_s)));
        look(u"another than was said yes to"_s);
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30:8443"_s, true);
        QTRY_COMPARE(trust->property("text").toString(), u"Trust Windows' word"_s);
        look(u"the word of Windows at a new address"_s);
        m_editing.began(u"saving"_s);
        look(u"the word of Windows at a new address, saving"_s);
        m_editing.gaveUp();
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // And one that was opened from a card, for the certificate alone.
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        QTRY_VERIFY(isSeen(certificates));
        look(u"from a card, reading"_s);
        m_editing.refused(u"Device is unreachable"_s);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        look(u"from a card, unreachable"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        look(u"from a card, two that differ"_s);
        m_editing.began(u"trusting"_s);
        look(u"from a card, two that differ, trusting"_s);
        m_editing.refused(shownAnother);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        look(u"from a card, two that differ, refused"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        // And where what is shown is the certificate of another KVM of the wall: no yes there.
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate(), u"Build server"_s);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"certificateWarning"_s)));
        look(u"from a card, the certificate of another KVM"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // The yes was there, whole inside the window; and where it stood, nothing else ever did.
        QVERIFY(yes.size() >= 9);
        QVERIFY(others.size() > yes.size());
        const QRectF inside(0, 0, window->width(), window->height());
        for (const Stood& one : std::as_const(yes)) {
            QVERIFY2(inside.contains(one.where), qPrintable(whereStood(one)));
            for (const Stood& other : std::as_const(others)) {
                QVERIFY2(!one.where.intersects(other.where),
                         qPrintable(whereStood(one) + u" is where there was "_s + whereStood(other)));
            }
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theYesToACertificateTakesNoClickForAMomentAfterItHasAppeared()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        QVERIFY(box != nullptr && name != nullptr && trust != nullptr && close != nullptr);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const auto clickAtOnce = [&](const QQuickItem* item) {
            // (Once the window has drawn the button, and no later: its place is laid out with a frame.)
            letTheWallComeToRest(window);
            QTest::mouseClick(window, Qt::LeftButton, {},
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };

        // The two arrive, and they differ: the button for the yes is there, and a click that
        // comes at once is no yes. The button looks as one that takes none.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        clickAtOnce(trust);
        QCOMPARE(trustAsked.count(), 0);
        QVERIFY(!trust->isEnabled());
        QVERIFY(trust->opacity() < 1);
        // A moment later it takes one, for exactly what is shown.
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        QCOMPARE(trust->opacity(), 1.0);
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 1);
        QCOMPARE(trustAsked[0][1].toByteArray(), anotherCertificate());
        m_editing.began(u"trusting"_s);
        m_editing.gaveUp();
        QTRY_VERIFY(trust->isEnabled());

        // What it would say yes to changes: the two are read again, and it shows a third one by
        // now. The moment begins anew.
        const QByteArray third =
            QByteArray::fromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        clickOn(window, close);
        QTRY_VERIFY(isSeen(name));
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), third);
        QTRY_VERIFY(isSeen(trust));
        clickAtOnce(trust);
        QCOMPARE(trustAsked.count(), 1);
        QVERIFY(!trust->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 2);
        QCOMPARE(trustAsked[1][1].toByteArray(), third);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theYesToACertificateIsNotReachedByTheKeyboard()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && trust != nullptr);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QString shownAnother = u"The device now presents a different certificate than the one shown"_s;

        // Every state of the box in which the button for the yes is there.
        const QList<std::pair<QString, std::function<void()>>> states{
            {u"looked at from the lines"_s,
             [&] {
                 m_editing.show(aKvmOfTheWall(), false);
                 m_editing.began(u"looking"_s);
                 m_editing.reviewed(oneCertificate(), anotherCertificate());
             }},
            {u"looked at from a card"_s,
             [&] {
                 m_editing.show(aKvmOfTheWall(), true);
                 m_editing.began(u"looking"_s);
                 m_editing.reviewed(oneCertificate(), anotherCertificate());
             }},
            {u"after a yes that was refused"_s,
             [&] {
                 m_editing.show(aKvmOfTheWall(), false);
                 m_editing.began(u"looking"_s);
                 m_editing.reviewed(oneCertificate(), anotherCertificate());
                 m_editing.began(u"trusting"_s);
                 m_editing.refused(shownAnother);
             }},
            {u"shown for an address that was typed"_s,
             [&] {
                 m_editing.show(aKvmOfTheWall(), false);
                 m_editing.began(u"saving"_s);
                 m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
             }},
        };
        // Tab as many times as there are things to stop at in the box, and some more, which
        // comes round to each a second time: it never stops at that button. And wherever it has
        // stopped, the space bar, Return and Enter are no yes.
        const int mostStops = 5;
        for (const auto& [state, make] : states) {
            for (int stops = 0; stops <= mostStops; ++stops) {
                const QByteArray where = (state + u", Tab %1 times"_s.arg(stops)).toUtf8();
                if (box->property("opened").toBool()) {
                    QVERIFY(QMetaObject::invokeMethod(box, "close"));
                }
                QTRY_VERIFY2(!box->property("opened").toBool(), where.constData());
                make();
                QTRY_VERIFY2(box->property("opened").toBool(), where.constData());
                // (Once it takes a click: a key that did nothing before that would show nothing.)
                QTRY_VERIFY2_WITH_TIMEOUT(isSeen(trust) && trust->isEnabled(), where.constData(), 5000);
                letTheWallComeToRest(window);
                for (int stop = 0; stop < stops; ++stop) {
                    QTest::keyClick(window, Qt::Key_Tab);
                    QVERIFY2(!trust->hasActiveFocus(), where.constData());
                }
                for (const Qt::Key key : {Qt::Key_Space, Qt::Key_Return, Qt::Key_Enter}) {
                    // (The way back is a button the keyboard does reach: pressed there, the two
                    // are gone, and the button for the yes with them.)
                    if (isSeen(trust)) {
                        QTest::keyClick(window, key);
                    }
                    QVERIFY2(trustAsked.isEmpty(), where.constData());
                    // (Back at the lines, Return saves what stands in them: with no yes to
                    // anything.)
                    for (const QList<QVariant>& saving : saveAsked) {
                        QVERIFY2(saving.at(6).toByteArray().isEmpty(), where.constData());
                    }
                }
                for (int stop = 0; isSeen(trust) && stop <= stops; ++stop) {
                    QTest::keyClick(window, Qt::Key_Backtab);
                    QVERIFY2(!trust->hasActiveFocus(), where.constData());
                }
            }
        }
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatTookThePlaceOfWhatWasSaidYesToIsSaidInTheBoxAndMarkedAndTheYesRestsForLonger_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void whatTookThePlaceOfWhatWasSaidYesToIsSaidInTheBoxAndMarkedAndTheYesRestsForLonger()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* another = window->findChild<QQuickItem*>(u"certificateAnotherNow"_s);
        const auto* plain = window->findChild<QQuickItem*>(u"editPlain"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && lines != nullptr && remembered != nullptr && presented != nullptr);
        QVERIFY(warning != nullptr && plain != nullptr && close != nullptr && trust != nullptr);
        QVERIFY2(another != nullptr, "the box has nothing that says so");
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto isInSight = [&](const QQuickItem* item) {
            return isSeen(item) && inside.contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())));
        };
        const auto clickAtOnce = [&](const QQuickItem* item) {
            letTheWallComeToRest(window);
            QTest::mouseClick(window, Qt::LeftButton, {},
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        const QString sizes = u"-%1x%2"_s.arg(size.width()).arg(size.height());
        const QString saysSo =
            u"What answers at that address shows another certificate now than the one that was just said yes to."_s;
        const QByteArray third =
            QByteArray::fromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        const QString thirdInLines =
            u"01:23:45:67:89:AB:CD:EF\n01:23:45:67:89:AB:CD:EF\n01:23:45:67:89:AB:CD:EF\n01:23:45:67:89:AB:CD:EF"_s;

        // A saving is refused over the certificate shown at the address that was typed; the two
        // are looked at, and the yes is clicked. Nothing says that anything changed, so far.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30:8443"_s);
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(!isSeen(another));
        QVERIFY(!presented->property("marked").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked[0][6].toByteArray(), anotherCertificate());
        m_editing.began(u"saving"_s);

        // What answers there shows a third one by then. The box says so, in the warning's
        // colour, in the place of the note that was read before; the one that changed is marked
        // as changed, and the other is not.
        QElapsedTimer since;
        since.start();
        m_editing.differing(oneCertificate(), third, u"192.168.1.30:8443"_s);
        QTRY_VERIFY(isSeen(another));
        QCOMPARE(another->property("text").toString(), saysSo);
        QCOMPARE(another->property("color").value<QColor>(), plain->property("color").value<QColor>());
        QVERIFY(another->property("font").value<QFont>().weight() >= QFont::DemiBold);
        QVERIFY(!warning->isVisible());
        QVERIFY(says(remembered, oneInLines()));
        QVERIFY(says(presented, thirdInLines));
        QVERIFY(presented->property("marked").toBool());
        QVERIFY(!remembered->property("marked").toBool());
        QVERIFY(says(presented, u"Changed. Presented right now at the new address, 192.168.1.30:8443:"_s));
        QVERIFY(says(remembered, u"Remembered by qKVM:"_s));
        // All of it is in sight at once, whatever the window's size.
        letTheWallComeToRest(window);
        for (const QQuickItem* item : {another, remembered, presented, close, trust}) {
            QVERIFY2(isInSight(item), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        QVERIFY(another->height() >= another->implicitHeight() - 0.5);
        keepPictureOf(window, u"edit-kvm-another-than-said-yes-to"_s + sizes);
        // The yes takes no click, and looks so: not at once, and not after its usual moment.
        QVERIFY(isSeen(trust) && !trust->isEnabled());
        clickAtOnce(trust);
        QTest::qWait(qkvm::ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!trust->isEnabled());
        QVERIFY(trust->opacity() < 1);
        clickAtOnce(trust);
        QCOMPARE(saveAsked.count(), 1);
        // Only after three times as long; and then it is a yes to the third.
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 8000);
        QVERIFY2(since.elapsed() >= qkvm::ui::EditKvmView::trustRestsAnewMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        QVERIFY(isSeen(another));
        keepPictureOf(window, u"edit-kvm-another-than-said-yes-to-after-its-rest"_s + sizes);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 2);
        QCOMPARE(saveAsked[1][6].toByteArray(), third);
        m_editing.began(u"saving"_s);
        m_editing.gaveUp();
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // So where a certificate is looked at, at the address the KVM has: the yes is to one
        // that it shows no longer.
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(!isSeen(another));
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 1);
        m_editing.began(u"trusting"_s);
        since.restart();
        m_editing.reviewed(oneCertificate(), third);
        QTRY_VERIFY(isSeen(another));
        QCOMPARE(another->property("text").toString(), saysSo);
        QVERIFY(!warning->isVisible());
        QVERIFY(says(presented, thirdInLines));
        QVERIFY(presented->property("marked").toBool());
        QVERIFY(says(presented, u"Changed. Presented by the device right now:"_s));
        QVERIFY(!isSeen(window->findChild<QQuickItem*>(u"editProblem"_s)));
        letTheWallComeToRest(window);
        for (const QQuickItem* item : {another, remembered, presented, close, trust}) {
            QVERIFY2(isInSight(item), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        keepPictureOf(window, u"edit-kvm-review-another-than-said-yes-to"_s + sizes);
        QVERIFY(!trust->isEnabled());
        clickAtOnce(trust);
        QTest::qWait(qkvm::ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!trust->isEnabled());
        clickAtOnce(trust);
        QCOMPARE(trustAsked.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 8000);
        QVERIFY2(since.elapsed() >= qkvm::ui::EditKvmView::trustRestsAnewMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 2);
        QCOMPARE(trustAsked[1][1].toByteArray(), third);
        m_editing.began(u"trusting"_s);
        m_editing.gaveUp();
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatAKvmWouldBeKnownByAtAnAddressThatWasTypedIsShownBesideWhatItIsKnownByNow_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void whatAKvmWouldBeKnownByAtAnAddressThatWasTypedIsShownBesideWhatItIsKnownByNow()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && lines != nullptr && remembered != nullptr && presented != nullptr);
        QVERIFY(warning != nullptr && close != nullptr && trust != nullptr);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        // Everything that says what a yes would be given to is in sight at once, and whole.
        const auto allInSight = [&] {
            letTheWallComeToRest(window);
            for (const QQuickItem* item : {remembered, presented, warning, close, trust}) {
                if (!isSeen(item) || !inside.contains(placeOf(item))) {
                    return false;
                }
            }
            for (const QQuickItem* one : {remembered, presented}) {
                QList<const QQuickItem*> written;
                this->lines(one, written);
                for (const QQuickItem* line : std::as_const(written)) {
                    if (line->isVisible() && (line->property("contentWidth").toReal() > line->width() + 0.5 ||
                                              line->height() < line->implicitHeight() - 0.5 ||
                                              !placeOf(one).adjusted(-0.5, -0.5, 0.5, 0.5).contains(placeOf(line)))) {
                        return false;
                    }
                }
            }
            return lines->property("contentHeight").toReal() <= lines->height() + 1;
        };
        const QString sizes = u"-%1x%2"_s.arg(size.width()).arg(size.height());
        const QString windowsTrusts = u"Windows trusts what is shown there for this name"_s;
        qkvm::ui::EditKvmView::Shown inTheClear = aKvmOfTheWall();
        inTheClear.address = u"http://192.168.1.22:8080"_s;
        inTheClear.certificate.clear();
        inTheClear.https = false;
        inTheClear.overPlainHttp = true;

        // A KVM that is known by none yet (it is spoken to in the clear), and a certificate is
        // shown at the address that was typed for it: "none yet" beside that certificate, and
        // what a yes to it means.
        m_editing.show(inTheClear, false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing({}, oneCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(says(remembered, u"Remembered by qKVM:"_s));
        QVERIFY(says(remembered, u"none yet"_s));
        QVERIFY(says(presented, u"Presented right now at the new address, 192.168.1.30:"_s));
        QVERIFY(says(presented, oneInLines()));
        QCOMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(),
                 u"This KVM is known by no certificate yet, so there is none here to compare this one with: compare "
                 "it with what the KVM's own page shows. Saying yes sends the password to what shows it, and the KVM "
                 "is known by it from then on."_s);
        QVERIFY(allInSight());
        keepPictureOf(window, u"edit-kvm-known-by-none-and-a-certificate-there"_s + sizes);
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked[0][6].toByteArray(), oneCertificate());
        m_editing.began(u"saving"_s);
        m_editing.gaveUp();
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // A KVM that is known by a certificate, and Windows trusts what is shown at the address
        // that was typed: the certificate beside that, said in the place of a second one, and
        // that a yes means it is known by Windows' word from then on and no longer by its
        // certificate.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"kvm.example.com"_s, true);
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(says(remembered, oneInLines()));
        QVERIFY(says(presented, u"Presented right now at the new address, kvm.example.com:"_s));
        QVERIFY(says(presented, windowsTrusts));
        QVERIFY(!says(presented, anotherInLines()));
        // (No certificate stands there to say yes to: the button says what the yes is to.)
        QCOMPARE(trust->property("text").toString(), u"Trust Windows' word"_s);
        QVERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(),
                 u"Saying yes means that this KVM is known by Windows' word from then on, and no longer by its "
                 "certificate."_s);
        QVERIFY(allInSight());
        keepPictureOf(window, u"edit-kvm-known-by-a-certificate-and-windows-word-there"_s + sizes);
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 2);
        // (The certificate that was shown there goes with the yes; that it is Windows' word
        // for it that was said yes to, the program knows from what it told the box to show.)
        QCOMPARE(saveAsked[1][6].toByteArray(), anotherCertificate());
        m_editing.began(u"saving"_s);
        // What answers there by then shows another certificate, which Windows trusts as well:
        // said, together with what a yes means, and all of it in sight.
        const QByteArray third =
            QByteArray::fromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        m_editing.differing(oneCertificate(), third, u"kvm.example.com"_s, true);
        const auto* another = window->findChild<QQuickItem*>(u"certificateAnotherNow"_s);
        QVERIFY(another != nullptr);
        QTRY_VERIFY(isSeen(another));
        // (This said "shows another certificate now than the one that was just said yes to":
        // none was shown, and it was Windows' word that was said yes to. Turned round.)
        QCOMPARE(another->property("text").toString(),
                 u"What answers at that address is not what it was when Windows' word for it was said yes to. "
                 "Windows trusts what it shows now, too. Saying yes means that this KVM is known by Windows' word "
                 "from then on, and no longer by its certificate."_s);
        QVERIFY(says(presented, windowsTrusts));
        QVERIFY(presented->property("marked").toBool());
        QVERIFY(!trust->isEnabled());
        letTheWallComeToRest(window);
        for (const QQuickItem* item : {another, remembered, presented, close, trust}) {
            QVERIFY2(isSeen(item) && inside.contains(placeOf(item)), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        keepPictureOf(window, u"edit-kvm-windows-word-in-the-place-of-what-was-said-yes-to"_s + sizes);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // And one that is spoken to in the clear, where Windows trusts what is shown there:
        // "none yet" beside that, and it has no certificate to give up.
        m_editing.show(inTheClear, false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing({}, anotherCertificate(), u"kvm.example.com"_s, true);
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(says(remembered, u"none yet"_s));
        QVERIFY(says(presented, windowsTrusts));
        QCOMPARE(warning->property("text").toString(),
                 u"Saying yes means that this KVM is known by Windows' word from then on."_s);
        QVERIFY(allInSight());
        keepPictureOf(window, u"edit-kvm-known-by-none-and-windows-word-there"_s + sizes);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aKvmThatIsKnownByWindowsWordIsShownAsThatBesideTheCertificateThatWouldTakeItsPlace()
    {
        // Spoken to over HTTPS and known by no certificate of its own: Windows vouches for it. At
        // the address that was typed a certificate is shown that Windows does not trust. What the
        // KVM is known by now stands beside it, and what a yes gives up is said. (This said "none
        // yet" and "known by no certificate yet", as of a KVM that is spoken to in the clear: a
        // yes then dropped Windows' word without a word of it.)
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(640, 400);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && remembered != nullptr && presented != nullptr && warning != nullptr);
        QVERIFY(trust != nullptr);
        qkvm::ui::EditKvmView::Shown onWindowsWord = aKvmOfTheWall();
        onWindowsWord.address = u"kvm.example.com"_s;
        onWindowsWord.certificate.clear();

        m_editing.show(onWindowsWord, false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing({}, oneCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(says(remembered, u"Remembered by qKVM:"_s));
        QVERIFY(says(remembered, u"Windows' word for the certificate it shows"_s));
        QVERIFY(!says(remembered, u"none yet"_s));
        QVERIFY(says(presented, u"Presented right now at the new address, 192.168.1.30:"_s));
        QVERIFY(says(presented, oneInLines()));
        QCOMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(),
                 u"This KVM is known by Windows' word and by no certificate of its own, so there is none here to "
                 "compare this one with: compare it with what the KVM's own page shows. Saying yes sends the password "
                 "to what shows it, and the KVM is known by this certificate from then on, and no longer by Windows' "
                 "word."_s);
        letTheWallComeToRest(window);
        keepPictureOf(window, u"edit-kvm-known-by-windows-word-and-a-certificate-there-640x400"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theYesToACertificateRestsWhileItsWindowIsNotTheActiveOne()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && trust != nullptr);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const auto clickAtOnce = [&](const QQuickItem* item) {
            QTest::mouseClick(window, Qt::LeftButton, {},
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };

        // Somebody saves, and goes to another program while the KVM is asked: another window
        // is the active one when the two certificates arrive.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        QWindow elsewhere;
        elsewhere.resize(200, 100);
        elsewhere.show();
        elsewhere.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        QVERIFY(!window->isActive());
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(isSeen(trust));
        // The moment does not pass with nobody looking: however long the two are there, the
        // button takes no click, and a yes is not passed on.
        letTheWallComeToRest(window);
        QTest::qWait(qkvm::ui::EditKvmView::trustRestsMs + 500);
        QVERIFY(!m_editing.isLookedAt());
        QVERIFY(!m_editing.mayTrust());
        QVERIFY(!trust->isEnabled());
        clickAtOnce(trust);
        QCOMPARE(saveAsked.count(), 0);
        // The window is the active one again: the moment begins then. The click that brought
        // it back, landing on the button, is no yes.
        QElapsedTimer since;
        since.start();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(m_editing.isLookedAt());
        QVERIFY(!trust->isEnabled());
        clickAtOnce(trust);
        QCOMPARE(saveAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        QVERIFY2(since.elapsed() >= qkvm::ui::EditKvmView::trustRestsMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        // Away again, and back: from the beginning. (The program takes no yes from the moment
        // it is told; the button looks so a turn of the loop later.)
        elsewhere.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        QTRY_VERIFY(!m_editing.isLookedAt());
        QVERIFY(!m_editing.mayTrust());
        clickAtOnce(trust);
        QCOMPARE(saveAsked.count(), 0);
        QTRY_VERIFY(!trust->isEnabled());
        since.restart();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(!m_editing.mayTrust());
        QVERIFY(!trust->isEnabled());
        clickAtOnce(trust);
        QCOMPARE(saveAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        QVERIFY2(since.elapsed() >= qkvm::ui::EditKvmView::trustRestsMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked[0][6].toByteArray(), anotherCertificate());
        m_editing.began(u"saving"_s);
        m_editing.gaveUp();
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        // A box that is closed is looked at by nobody.
        QVERIFY(!m_editing.isLookedAt());

        // So with a look at its certificate, in a box that was opened for that alone.
        elsewhere.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        QTest::qWait(qkvm::ui::EditKvmView::trustRestsMs + 500);
        QVERIFY(!trust->isEnabled());
        clickAtOnce(trust);
        QCOMPARE(trustAsked.count(), 0);
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        QCOMPARE(trustAsked.count(), 1);
        m_editing.began(u"trusting"_s);
        m_editing.gaveUp();
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatShowsTheCertificateOfAnotherKvmOfTheWallIsSaidInTheBoxWhichHasNoYesForIt_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void whatShowsTheCertificateOfAnotherKvmOfTheWallIsSaidInTheBoxWhichHasNoYesForIt()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* certificates = window->findChild<QQuickItem*>(u"certificates"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && lines != nullptr && certificates != nullptr && remembered != nullptr);
        QVERIFY(presented != nullptr && warning != nullptr && close != nullptr && trust != nullptr);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto isInSight = [&](const QQuickItem* item) {
            return isSeen(item) && inside.contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())));
        };

        // Its card says that the certificate does not match. What answers at its address shows
        // the certificate another KVM of the wall is known by: said beside the two, before
        // anybody compares them, with what is to be done about it.
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate(), u"Build server"_s);
        QTRY_VERIFY(isSeen(warning));
        QVERIFY(says(remembered, oneInLines()));
        QVERIFY(says(presented, anotherInLines()));
        QCOMPARE(warning->property("text").toString(),
                 u"What answers at this address shows the certificate of Build server, which is on this wall. Either "
                 "that KVM answers here now, or the two were given the same certificate: then each needs one of its "
                 "own before both can be on the wall."_s);
        // There is no yes to it: the button is not there, however long the two are looked at,
        // and the way out says that it closes, not that it gives something up.
        QTest::qWait(qkvm::ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!isSeen(trust));
        QVERIFY(!m_editing.mayTrust());
        QCOMPARE(close->property("text").toString(), u"Close"_s);
        QTRY_VERIFY(certificates->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(trustAsked.count(), 0);
        letTheWallComeToRest(window);
        for (const QQuickItem* item : {remembered, presented, warning, close}) {
            QVERIFY2(isInSight(item), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        keepPictureOf(window, u"edit-kvm-review-another-kvms-certificate-%1x%2"_s.arg(size.width()).arg(size.height()));
        clickOn(window, close);
        QTRY_VERIFY(!box->property("opened").toBool());

        // A certificate that is nobody else's has its yes, as ever.
        m_editing.show(aKvmOfTheWall(), true);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBoxesOfAWindowThatIsReallyOutOfSightAreClosedAndEmptied()
    {
        // The real screens, in a window that is really hidden, as the last window is behind the
        // program's icon. Whoever keeps the windows closes its boxes then (WallWindows, whose
        // part is taken here by the test): the screens close them and empty what was typed,
        // though nobody sees them do it.
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        m_editing.closed();
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* editBox = window->findChild<QObject*>(u"editKvm"_s);
        QObject* addBox = window->findChild<QObject*>(u"addKvm"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* password = window->findChild<QQuickItem*>(u"editPassword"_s);
        const auto* newAddress = window->findChild<QQuickItem*>(u"kvmAddress"_s);
        const auto* newPassword = window->findChild<QQuickItem*>(u"kvmPassword"_s);
        QVERIFY(editBox != nullptr && addBox != nullptr && name != nullptr && password != nullptr);
        QVERIFY(newAddress != nullptr && newPassword != nullptr);
        const QSignalSpy giveUpAsked(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const auto outOfSight = connect(window, &QWindow::visibleChanged, this, [this](bool isVisible) {
            if (!isVisible) {
                m_adding.close();
                m_editing.close();
            }
        });

        // The box that changes a KVM, with a password typed into it and a saving under way.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(editBox->property("opened").toBool());
        QTRY_VERIFY(name->property("hasKeyboard").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        m_editing.began(u"saving"_s);
        window->hide();
        QVERIFY(!window->isVisible());
        QTRY_VERIFY(!editBox->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());
        QCOMPARE(name->property("text").toString(), QString());
        QCOMPARE(m_editing.machine(), QString());
        QVERIFY(!m_editing.busy());
        QCOMPARE(giveUpAsked.count(), 1);
        QCOMPARE(giveUpAsked[0][0].toBool(), true);
        // What comes for it afterwards is nobody's; and brought back, the window shows no box.
        m_editing.saved(u"NAS"_s);
        QVERIFY(bringUp(window));
        QTest::qWait(100);
        QVERIFY(!editBox->property("opened").toBool());
        QCOMPARE(password->property("text").toString(), QString());

        // The two certificates of a saving that was refused over them, in a window that goes
        // out of sight: gone, and no yes is taken afterwards.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(editBox->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30"_s);
        QTRY_VERIFY(isSeen(window->findChild<QQuickItem*>(u"reviewTrust"_s)));
        window->hide();
        QTRY_VERIFY(!editBox->property("opened").toBool());
        QVERIFY(!m_editing.differs());
        QCOMPARE(m_editing.presented(), QString());
        m_editing.saveKnownByWhatIsShown(u"NAS"_s, u"192.168.1.30"_s, u"admin"_s, u"secret"_s, QString());
        QCOMPARE(saveAsked.count(), 0);
        QVERIFY(bringUp(window));

        // The box that adds one, with an address and a password typed into it.
        QVERIFY(QMetaObject::invokeMethod(addBox, "open"));
        QTRY_VERIFY(addBox->property("opened").toBool());
        type(window, u"192.168.1.20"_s);
        QTest::keyClick(window, Qt::Key_Return);
        type(window, u"a password"_s);
        QCOMPARE(newPassword->property("text").toString(), u"a password"_s);
        window->hide();
        QTRY_VERIFY(!addBox->property("opened").toBool());
        QCOMPARE(newPassword->property("text").toString(), QString());
        QCOMPARE(newAddress->property("text").toString(), QString());
        QVERIFY(bringUp(window));
        QTest::qWait(100);
        QVERIFY(!addBox->property("opened").toBool());
        disconnect(outOfSight);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatABoxHadDoneAsItsWindowWentOutOfSightIsToldWhenTheWindowIsBack()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        m_adding.reset();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QObject* editBox = window->findChild<QObject*>(u"editKvm"_s);
        QObject* addBox = window->findChild<QObject*>(u"addKvm"_s);
        QVERIFY(editBox != nullptr && addBox != nullptr);
        // Whoever keeps the windows (WallWindows, whose part the test takes): a window that goes
        // out of sight has its boxes closed, and what is told for when it is seen waits.
        const auto outOfSight = connect(window, &QWindow::visibleChanged, this, [this, &actions](bool isVisible) {
            actions.notices()->setSeen(isVisible);
            if (!isVisible) {
                m_adding.close();
                m_editing.close();
            }
        });
        // And whoever changes the KVM (OwnWall, likewise): it had saved it in that very turn,
        // at another address, where it is known by another certificate; and says so to the box
        // as the box closes over it.
        const auto asItCloses = connect(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked, this, [this](bool closing) {
            if (closing) {
                m_editing.savedAsItClosed(u"Storage"_s, true, writtenAnother(), false);
            }
        });
        const QString savedAndKnownBy = u"Storage saved. Remembered SHA-256: "_s + writtenAnother();

        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(editBox->property("opened").toBool());
        m_editing.began(u"saving"_s);
        window->hide();
        QVERIFY(!window->isVisible());
        QTRY_VERIFY(!editBox->property("opened").toBool());
        // Said, as a notice of that window, in the page's words: that it was saved, and what
        // the box would have stayed open to show.
        QCOMPARE(toldBy(actions), QStringList{u"ok: "_s + savedAndKnownBy});
        // It is there when the window is back, and is drawn whole in its notice.
        QTest::qWait(300);
        QVERIFY(bringUp(window));
        // (Drawn with the certificate in lines of eight pairs, as its box shows one: this
        // looked for the sentence as it was told, which was broken wherever its line ended.)
        const QString drawn = u"Storage saved. Remembered SHA-256:\n"_s + anotherInLines();
        QTRY_VERIFY(saying(window->contentItem(), drawn) != nullptr);
        const QQuickItem* told = saying(window->contentItem(), drawn);
        QVERIFY(told != nullptr);
        letTheWallComeToRest(window);
        QVERIFY(told->property("contentWidth").toReal() <= told->width() + 0.5);
        QVERIFY(isInside(window, told->parentItem()));
        QVERIFY(told->parentItem()->height() >= told->height());
        QVERIFY(!editBox->property("opened").toBool());
        keepPictureOf(window, u"edit-kvm-saved-as-its-window-went"_s);
        disconnect(asItCloses);
        actions.notices()->clear();

        // Saved to where it is spoken to in the clear; saved with nothing to be seen of it; and
        // a certificate that was trusted.
        const auto tellsWhenClosedOver = [&](const std::function<void()>& done) {
            m_editing.show(aKvmOfTheWall(), false);
            if (!QTest::qWaitFor([&] { return editBox->property("opened").toBool(); })) {
                return QStringList{u"the box did not open"_s};
            }
            m_editing.began(u"saving"_s);
            const auto closing = connect(&m_editing, &qkvm::ui::EditKvmView::giveUpAsked, this, done);
            window->hide();
            disconnect(closing);
            const QStringList told = toldBy(actions);
            actions.notices()->clear();
            return bringUp(window) ? told : QStringList{u"the window did not come back"_s};
        };
        QCOMPARE(tellsWhenClosedOver([this] { m_editing.savedAsItClosed(u"Storage"_s, true, QString(), true); }),
                 QStringList{u"ok: Storage saved. This KVM is spoken to over plain HTTP: its password, and what is "
                             "seen and typed there, can be read on the way."_s});
        QCOMPARE(tellsWhenClosedOver([this] { m_editing.savedAsItClosed(u"Storage"_s, true, QString(), false); }),
                 QStringList{u"ok: Storage saved. No certificate remembered yet."_s});
        QCOMPARE(tellsWhenClosedOver([this] { m_editing.savedAsItClosed(u"Storage"_s, false, QString(), false); }),
                 QStringList{u"ok: Storage saved"_s});
        QCOMPARE(tellsWhenClosedOver([this] { m_editing.trustedAsItClosed(u"NAS"_s); }),
                 QStringList{u"ok: NAS: certificate trusted"_s});
        // (A name is told as it is, whatever is in it.)
        QCOMPARE(tellsWhenClosedOver([this] { m_editing.savedAsItClosed(u"Rack %2 %1"_s, true, QString(), false); }),
                 QStringList{u"ok: Rack %2 %1 saved. No certificate remembered yet."_s});

        // A KVM that was added as the window went: what the box would have said, and the
        // certificate it would have shown.
        QVERIFY(QMetaObject::invokeMethod(addBox, "open"));
        QTRY_VERIFY(addBox->property("opened").toBool());
        m_adding.began();
        window->hide();
        QTRY_VERIFY(!addBox->property("opened").toBool());
        m_adding.took(u"192.168.1.20 added"_s, writtenOne());
        const QString addedAndKnownBy = u"192.168.1.20 added. Remembered SHA-256: "_s + writtenOne();
        QCOMPARE(toldBy(actions), QStringList{u"ok: "_s + addedAndKnownBy});
        QCOMPARE(m_adding.count(), 0);
        QTest::qWait(300);
        QVERIFY(bringUp(window));
        QTRY_VERIFY(saying(window->contentItem(), u"192.168.1.20 added. Remembered SHA-256:\n"_s + oneInLines()) !=
                    nullptr);
        QVERIFY(!addBox->property("opened").toBool());
        keepPictureOf(window, u"add-kvm-added-as-its-window-went"_s);
        actions.notices()->clear();
        // And one that is spoken to in the clear shows none: what the box would have said.
        QVERIFY(QMetaObject::invokeMethod(addBox, "open"));
        QTRY_VERIFY(addBox->property("opened").toBool());
        m_adding.began();
        window->hide();
        m_adding.took(u"192.168.1.20 added"_s);
        QCOMPARE(toldBy(actions), QStringList{u"ok: 192.168.1.20 added"_s});
        QVERIFY(bringUp(window));
        disconnect(outOfSight);
        actions.notices()->clear();
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatIsGivenUpAtAPlainHttpAddressIsShownBesideWhatTheKvmIsKnownByAndTakesAYesOfItsOwn_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void whatIsGivenUpAtAPlainHttpAddressIsShownBesideWhatTheKvmIsKnownByAndTakesAYesOfItsOwn()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* certificates = window->findChild<QQuickItem*>(u"certificates"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* another = window->findChild<QQuickItem*>(u"certificateAnotherNow"_s);
        const auto* plain = window->findChild<QQuickItem*>(u"editPlain"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && lines != nullptr && name != nullptr && certificates != nullptr);
        QVERIFY(remembered != nullptr && presented != nullptr && warning != nullptr && another != nullptr);
        QVERIFY(plain != nullptr && close != nullptr && trust != nullptr);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QSignalSpy trustAsked(&m_editing, &qkvm::ui::EditKvmView::trustAsked);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto placeOf = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        // The sentence that stands where a certificate would, as it is drawn: null where it is
        // not there, or is not whole in its frame.
        const auto wholeIn = [&](const QQuickItem* half, const QString& sentence) -> const QQuickItem* {
            const QQuickItem* line = saying(half, sentence);
            const bool whole = line != nullptr && line->property("contentWidth").toReal() <= line->width() + 0.5 &&
                               line->height() >= line->implicitHeight() - 0.5 &&
                               placeOf(half).adjusted(-0.5, -0.5, 0.5, 0.5).contains(placeOf(line));
            return whole ? line : nullptr;
        };
        const QString sizes = u"-%1x%2"_s.arg(size.width()).arg(size.height());
        const QString givenUp = u"Plain HTTP at 192.168.1.30:8080: nothing shows who answers there, and its "
                                "password, and what is seen and typed, can be read on the way"_s;

        // A KVM that is known by a certificate, and the address that was typed for it is plain
        // HTTP: the certificate beside what is given up there, in the warning's colour, with
        // nothing over it that says something was presented; and the yes says what it is to.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.givingUpHttps(oneCertificate(), u"192.168.1.30:8080"_s);
        QTRY_VERIFY(isSeen(trust));
        QVERIFY(says(window->contentItem(), u"\"NAS\" certificate"_s));
        QVERIFY(!isSeen(name));
        QVERIFY(says(remembered, u"Remembered by qKVM:"_s));
        QVERIFY(says(remembered, oneInLines()));
        letTheWallComeToRest(window);
        const QQuickItem* sentence = wholeIn(presented, givenUp);
        QVERIFY2(sentence != nullptr, "what is given up is not said, or not whole");
        QCOMPARE(sentence->property("color").value<QColor>(), plain->property("color").value<QColor>());
        QVERIFY(!says(presented, u"Presented right now at the new address, 192.168.1.30:8080:"_s));
        QVERIFY(!presented->property("marked").toBool());
        QCOMPARE(trust->property("text").toString(), u"Use plain HTTP"_s);
        QVERIFY(!trust->property("primary").toBool());
        QCOMPARE(close->property("text").toString(), u"Cancel"_s);
        QVERIFY(!warning->isVisible() && !isSeen(another));
        // All of it in sight at once, whatever the window's size.
        for (const QQuickItem* item : {remembered, presented, close, trust}) {
            QVERIFY2(isSeen(item) && inside.contains(placeOf(item)), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        QCOMPARE(placeOf(remembered).bottom(), placeOf(presented).bottom());
        QVERIFY(placeOf(remembered).right() < placeOf(presented).left());
        keepPictureOf(window, u"edit-kvm-plain-http-there"_s + sizes);
        // The yes rests as the others do, and no key is one: a click on its button, once it
        // takes one, asks for the saving of what stands in the lines, with no certificate.
        QVERIFY(!trust->isEnabled());
        QTest::mouseClick(window, Qt::LeftButton, {},
                          trust->mapToScene(QPointF(trust->width() / 2, trust->height() / 2)).toPoint());
        QTRY_VERIFY(certificates->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(saveAsked.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Space);
        QCOMPARE(saveAsked.count(), 0);
        clickOn(window, trust);
        QCOMPARE(saveAsked.count(), 1);
        QCOMPARE(saveAsked[0][0].toString(), u"0a1b2c3d"_s);
        QCOMPARE(saveAsked[0][6].toByteArray(), QByteArray());
        QCOMPARE(trustAsked.count(), 0);
        m_editing.began(u"saving"_s);
        QTRY_COMPARE(trust->property("text").toString(), u"Checking…"_s);
        m_editing.gaveUp();
        QTRY_COMPARE(trust->property("text").toString(), u"Use plain HTTP"_s);
        // The way back leads to the lines, and nothing was asked.
        clickOn(window, close);
        QTRY_VERIFY(isSeen(name));
        QVERIFY(!certificates->isVisible() && !trust->isVisible());
        QVERIFY(!m_editing.differs());
        QCOMPARE(saveAsked.count(), 1);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // One that is known by Windows' word, and by no certificate: that is what stands
        // beside what is given up, and it is not "none yet".
        qkvm::ui::EditKvmView::Shown vouchedFor = aKvmOfTheWall();
        vouchedFor.certificate.clear();
        m_editing.show(vouchedFor, false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.givingUpHttps({}, u"192.168.1.30:8080"_s);
        QTRY_VERIFY(isSeen(trust));
        letTheWallComeToRest(window);
        QVERIFY(wholeIn(remembered, u"Windows' word for the certificate it shows"_s) != nullptr);
        QVERIFY(!says(remembered, u"none yet"_s));
        QVERIFY(wholeIn(presented, givenUp) != nullptr);
        QCOMPARE(trust->property("text").toString(), u"Use plain HTTP"_s);
        QVERIFY(!warning->isVisible());
        for (const QQuickItem* item : {remembered, presented, close, trust}) {
            QVERIFY2(isSeen(item) && inside.contains(placeOf(item)), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        keepPictureOf(window, u"edit-kvm-plain-http-there-from-windows-word"_s + sizes);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // An address that has no blank to be broken at, and is longer than its half of the
        // box: whole all the same.
        const QString longAddress = u"kvm-in-the-second-rack-of-the-third-room.lab.example.com:8080"_s;
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.givingUpHttps(oneCertificate(), longAddress);
        QTRY_VERIFY(isSeen(trust));
        letTheWallComeToRest(window);
        QVERIFY(wholeIn(presented, u"Plain HTTP at %1: nothing shows who answers there, and its password, and what is "
                                   "seen and typed, can be read on the way"_s.arg(longAddress)) != nullptr);
        for (const QQuickItem* item : {remembered, presented, close, trust}) {
            QVERIFY2(isSeen(item) && inside.contains(placeOf(item)), qPrintable(item->objectName()));
        }
        QVERIFY(lines->property("contentHeight").toReal() <= lines->height() + 1);
        keepPictureOf(window, u"edit-kvm-plain-http-there-long-address"_s + sizes);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatIsShownAtALaterSavingIsHeldAgainstTheYesThatWasGivenForThatAddress_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void whatIsShownAtALaterSavingIsHeldAgainstTheYesThatWasGivenForThatAddress()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        m_editing.closed();
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        QObject* box = window->findChild<QObject*>(u"editKvm"_s);
        const auto* lines = window->findChild<QQuickItem*>(u"editLines"_s);
        const auto* name = window->findChild<QQuickItem*>(u"editName"_s);
        const auto* save = window->findChild<QQuickItem*>(u"editSave"_s);
        const auto* problem = window->findChild<QQuickItem*>(u"editProblem"_s);
        const auto* remembered = window->findChild<QQuickItem*>(u"certificateRemembered"_s);
        const auto* presented = window->findChild<QQuickItem*>(u"certificatePresented"_s);
        const auto* warning = window->findChild<QQuickItem*>(u"certificateWarning"_s);
        const auto* another = window->findChild<QQuickItem*>(u"certificateAnotherNow"_s);
        const auto* close = window->findChild<QQuickItem*>(u"reviewClose"_s);
        const auto* trust = window->findChild<QQuickItem*>(u"reviewTrust"_s);
        QVERIFY(box != nullptr && lines != nullptr && name != nullptr && save != nullptr && problem != nullptr);
        QVERIFY(remembered != nullptr && presented != nullptr && warning != nullptr && another != nullptr);
        QVERIFY(close != nullptr && trust != nullptr);
        const QSignalSpy saveAsked(&m_editing, &qkvm::ui::EditKvmView::saveAsked);
        const QRectF inside(0, 0, window->width(), window->height());
        const auto allInSight = [&] {
            letTheWallComeToRest(window);
            for (const QQuickItem* item : {another, remembered, presented, close, trust}) {
                if (!isSeen(item) ||
                    !inside.contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())))) {
                    return false;
                }
            }
            return lines->property("contentHeight").toReal() <= lines->height() + 1 &&
                   another->height() >= another->implicitHeight() - 0.5;
        };
        const QString sizes = u"-%1x%2"_s.arg(size.width()).arg(size.height());
        const QString atTheSignIn = u"The device now presents a different certificate than the one shown"_s;
        const QString windowsTrusts = u"Windows trusts what is shown there for this name"_s;
        const QByteArray third =
            QByteArray::fromHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        const QString thirdInLines =
            u"01:23:45:67:89:AB:CD:EF\n01:23:45:67:89:AB:CD:EF\n01:23:45:67:89:AB:CD:EF\n01:23:45:67:89:AB:CD:EF"_s;
        // A yes is clicked to what is shown; the sign-in is refused, and the box is back at its
        // lines, where it says why.
        const auto yesThenRefusedAtTheSignIn = [&] {
            if (!QTest::qWaitFor([&] { return isSeen(trust) && trust->isEnabled(); }, 8000)) {
                return false;
            }
            const qsizetype asked = saveAsked.count();
            clickOn(window, trust);
            if (saveAsked.count() != asked + 1) {
                return false;
            }
            m_editing.began(u"saving"_s);
            m_editing.refused(atTheSignIn);
            return QTest::qWaitFor([&] { return isSeen(name) && isSeen(problem); });
        };

        // A certificate was said yes to for an address; what answered the sign-in showed
        // another. Saved again, that other one is shown: marked, said in the words of a yes
        // that met another, and the yes rests for longer than its usual moment.
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"192.168.1.30:8443"_s);
        QVERIFY(yesThenRefusedAtTheSignIn());
        QCOMPARE(problem->property("text").toString(), atTheSignIn);
        clickOn(window, save);
        m_editing.began(u"saving"_s);
        QElapsedTimer since;
        since.start();
        m_editing.differing(oneCertificate(), third, u"192.168.1.30:8443"_s);
        QTRY_VERIFY(isSeen(another));
        QCOMPARE(another->property("text").toString(),
                 u"What answers at that address shows another certificate now than the one that was just said yes "
                 "to."_s);
        QVERIFY(!warning->isVisible());
        QVERIFY(presented->property("marked").toBool());
        QVERIFY(says(presented, u"Changed. Presented right now at the new address, 192.168.1.30:8443:"_s));
        QVERIFY(says(presented, thirdInLines));
        QVERIFY(allInSight());
        keepPictureOf(window, u"edit-kvm-another-at-the-next-saving"_s + sizes);
        QTest::qWait(qkvm::ui::EditKvmView::trustRestsMs + 300);
        QVERIFY(!trust->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 8000);
        QVERIFY2(since.elapsed() >= qkvm::ui::EditKvmView::trustRestsAnewMs - 100,
                 qPrintable(QString::number(since.elapsed())));
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // Windows' word was said yes to for an address, and no certificate stood there. What
        // is shown there at the next saving is one that Windows does not trust: said as that,
        // and not as "another certificate than the one that was said yes to".
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), anotherCertificate(), u"kvm.example.com"_s, true);
        QVERIFY(yesThenRefusedAtTheSignIn());
        clickOn(window, save);
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), third, u"kvm.example.com"_s);
        QTRY_VERIFY(isSeen(another));
        QCOMPARE(another->property("text").toString(),
                 u"Windows' word was said yes to for that address, and Windows does not trust what answers there "
                 "now. It shows this certificate."_s);
        QVERIFY(!warning->isVisible());
        QVERIFY(presented->property("marked").toBool());
        QVERIFY(says(presented, thirdInLines));
        QCOMPARE(trust->property("text").toString(), u"Trust this certificate"_s);
        QVERIFY(!trust->isEnabled());
        QVERIFY(allInSight());
        keepPictureOf(window, u"edit-kvm-after-windows-word-not-trusted"_s + sizes);
        // And where Windows trusts what is shown there now, which is another than it trusted
        // then: said as that, with what a yes means.
        clickOn(window, close);
        QTRY_VERIFY(isSeen(name));
        clickOn(window, save);
        m_editing.began(u"saving"_s);
        m_editing.differing(oneCertificate(), third, u"kvm.example.com"_s, true);
        QTRY_VERIFY(isSeen(another));
        QCOMPARE(another->property("text").toString(),
                 u"What answers at that address is not what it was when Windows' word for it was said yes to. "
                 "Windows trusts what it shows now, too. Saying yes means that this KVM is known by Windows' word "
                 "from then on, and no longer by its certificate."_s);
        QVERIFY(says(presented, windowsTrusts));
        QVERIFY(presented->property("marked").toBool());
        QCOMPARE(trust->property("text").toString(), u"Trust Windows' word"_s);
        QVERIFY(allInSight());
        keepPictureOf(window, u"edit-kvm-after-windows-word-another-trusted"_s + sizes);
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // A yes to a certificate that the KVM shows where it is, refused because the KVM is not
        // what the box was filled with any more: the box is back at its lines, filled anew, and
        // says why there.
        const QString changedMeanwhile =
            u"This device was changed meanwhile. Look at it again. The fields were filled in again with what is saved "
            "now."_s;
        qkvm::ui::EditKvmView::Shown renamed = aKvmOfTheWall();
        renamed.called = u"Storage"_s;
        renamed.inASentence = u"\"Storage\""_s;
        renamed.name = u"Storage"_s;
        m_editing.show(aKvmOfTheWall(), false);
        QTRY_VERIFY(box->property("opened").toBool());
        m_editing.began(u"looking"_s);
        m_editing.reviewed(oneCertificate(), anotherCertificate());
        QTRY_VERIFY(isSeen(trust));
        QTRY_VERIFY_WITH_TIMEOUT(trust->isEnabled(), 5000);
        clickOn(window, trust);
        m_editing.began(u"trusting"_s);
        m_editing.refusedOver(renamed, changedMeanwhile);
        QTRY_VERIFY(isSeen(name));
        QTRY_VERIFY(isSeen(problem));
        QCOMPARE(problem->property("text").toString(), changedMeanwhile);
        QCOMPARE(name->property("text").toString(), u"Storage"_s);
        QVERIFY(!isSeen(trust));
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aCertificateInANoticeStandsInLinesOfEightPairsAndIsBrokenInsideNone_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("in the smallest window") << QSize(640, 400);
        QTest::newRow("in a large window") << QSize(1280, 800);
    }

    void aCertificateInANoticeStandsInLinesOfEightPairsAndIsBrokenInsideNone()
    {
        QFETCH(QSize, size);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        QVERIFY(bringUp(window));
        const QString sizes = u"-%1x%2"_s.arg(size.width()).arg(size.height());
        const QString saved = u"Storage saved. Remembered SHA-256: "_s + writtenAnother();
        const QString woken = u"NAS: wake 00:11:22:33:44:55 sent"_s;

        // What a box says as its window goes, with the certificate the KVM is known by now. It
        // is told as one sentence, and drawn with the certificate in four lines of eight
        // pairs, as its box shows one: whole, and not one pair broken where a line ends.
        actions.tellWhenSeen(saved, true);
        actions.tell(woken, true);
        QCOMPARE(toldBy(actions), (QStringList{u"ok: "_s + saved, u"ok: "_s + woken}));
        const QString inLines = u"Storage saved. Remembered SHA-256:\n"_s + anotherInLines();
        QTRY_VERIFY2(saying(window->contentItem(), inLines) != nullptr,
                     "the certificate does not stand in lines of eight pairs");
        const QQuickItem* drawn = saying(window->contentItem(), inLines);
        letTheWallComeToRest(window);
        QCOMPARE(drawn->property("lineCount").toInt(), 5);
        QVERIFY(drawn->property("contentWidth").toReal() <= drawn->width() + 0.5);
        QVERIFY(isInside(window, drawn->parentItem()));
        QVERIFY(drawn->parentItem()->height() >= drawn->height());
        // What only looks a little like one is drawn as it was told.
        QVERIFY(saying(window->contentItem(), woken) != nullptr);
        keepPictureOf(window, u"notice-certificate-in-lines"_s + sizes);
        actions.notices()->clear();
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void atThisPcsOwnWallTheAccountLeadsToAServer()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy anotherServerAsked(&session, &qkvm::ui::SessionView::anotherServerAsked);
        const QSignalSpy signOutAsked(&session, &qkvm::ui::SessionView::signOutAsked);

        QObject* menu = window->findChild<QObject*>(u"accountMenu"_s);
        const QQuickItem* button = theOne(window, u"accountButton"_s);
        QVERIFY(menu != nullptr && button != nullptr);
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        // Nobody is signed in to anything: whose the wall is, and nothing of a server's. This PC's
        // wall as a file is not here: it is in the settings, in a section of its own
        // (test_settings_screens.cpp, test_own_files_screens.cpp).
        QCOMPARE(theOneSeen(window, u"accountWho"_s)->property("text").toString(), u"This PC"_s);
        QCOMPARE(
            theOneSeen(window, u"accountWhere"_s)->property("text").toString(),
            u"The KVMs on this wall were added on this PC, which speaks to them itself. No qKVM server is used."_s);
        const auto* signOut = window->findChild<QQuickItem*>(u"signOut"_s);
        const auto* toAServer = window->findChild<QQuickItem*>(u"connectToAServer"_s);
        QVERIFY(signOut != nullptr && toAServer != nullptr);
        QVERIFY(!signOut->isVisible());
        QVERIFY(!says(window->contentItem(), u"Export…"_s));
        QVERIFY(!says(window->contentItem(), u"Import…"_s));
        QVERIFY(toAServer->isVisible());
        keepPictureOf(window, u"account-own-wall"_s);
        clickOn(window, toAServer);
        QCOMPARE(anotherServerAsked.count(), 1);
        QCOMPARE(signOutAsked.count(), 0);
        QTRY_VERIFY(!menu->property("opened").toBool());

        // At a server's wall the same drop has the way out of the account, and not this one.
        session.show(signedIn());
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(signOut->isVisible());
        QVERIFY(!toAServer->isVisible());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whereAServerIsAskedForThereIsAWayToThisPcsOwnWall()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy ownWallAsked(&session, &qkvm::ui::SessionView::ownWallAsked);
        const QSignalSpy connectAsked(&session, &qkvm::ui::SessionView::connectAsked);

        const auto* way = window->findChild<QQuickItem*>(u"ownWall"_s);
        QVERIFY(way != nullptr);
        QVERIFY(way->isVisible());
        QCOMPARE(way->property("text").toString(), u"This PC's own KVMs"_s);
        clickOn(window, way);
        QCOMPARE(ownWallAsked.count(), 1);
        QCOMPARE(connectAsked.count(), 0);

        // Not while a server is being asked, and not where somebody is asked to sign in to one or
        // to look at its certificate: those have their own way back.
        qkvm::ui::SessionView::Shown shown;
        shown.busy = true;
        session.show(shown);
        QTRY_VERIFY(!way->isEnabled());
        shown.busy = false;
        shown.stage = u"signIn"_s;
        shown.address = u"192.168.1.10:8787"_s;
        session.show(shown);
        QTRY_VERIFY(!way->isVisible());
        shown.stage = u"trust"_s;
        shown.certificate = u"AB:CD"_s;
        session.show(shown);
        QTRY_VERIFY(!way->isVisible());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void beforeAnybodyIsSignedInTheWindowAsksWhichQkvmAndThenWho()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy connectAsked(&session, &qkvm::ui::SessionView::connectAsked);
        const QSignalSpy signInAsked(&session, &qkvm::ui::SessionView::signInAsked);

        // Which QKVM: no bar and no wall yet; the keyboard is in the address's line, and Enter asks.
        const auto* connectScreen = window->findChild<QQuickItem*>(u"connect"_s);
        QVERIFY(connectScreen != nullptr);
        QVERIFY(connectScreen->isVisible());
        QVERIFY(says(window->contentItem(), u"Address of your qKVM"_s));
        QVERIFY(!says(window->contentItem(), u"Nothing to show"_s));
        keepPictureOf(window, u"connect-address"_s);
        // Nothing typed, nothing asked.
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(connectAsked.count(), 0);
        type(window, u"192.168.1.10:8787"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(connectAsked.count(), 1);
        QCOMPARE(connectAsked[0][0].toString(), u"192.168.1.10:8787"_s);

        // While the server is asked, nothing more is; and what went wrong is said under the line.
        qkvm::ui::SessionView::Shown shown;
        shown.busy = true;
        session.show(shown);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(connectAsked.count(), 1);
        shown.busy = false;
        shown.problem = u"Nothing answers at that address."_s;
        session.show(shown);
        QTRY_VERIFY(says(window->contentItem(), u"Nothing answers at that address."_s));
        keepPictureOf(window, u"connect-problem"_s);

        // Who: a server with several accounts and a second step asks for all three, and the window
        // says that what is typed is not encrypted on its way.
        shown = {};
        shown.stage = u"signIn"_s;
        shown.address = u"192.168.1.10:8787"_s;
        shown.asksName = true;
        shown.asksCode = true;
        session.show(shown);
        QTRY_VERIFY(says(window->contentItem(), u"Sign in to 192.168.1.10:8787"_s));
        QVERIFY(says(window->contentItem(), u"User name"_s));
        QVERIFY(says(window->contentItem(), u"Password"_s));
        QVERIFY(says(window->contentItem(), u"Code from your authenticator app"_s));
        QVERIFY(!says(window->contentItem(), u"Address of your qKVM"_s));
        const auto* warning = window->findChild<QQuickItem*>(u"notEncrypted"_s);
        QVERIFY(warning != nullptr);
        QVERIFY(warning->isVisible());
        keepPictureOf(window, u"connect-sign-in"_s);
        // The keyboard is in the name's line; Enter there goes on to the password, Enter there signs in.
        type(window, u"anna"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(signInAsked.count(), 0);
        type(window, u"a password"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(signInAsked.count(), 1);
        QCOMPARE(signInAsked[0][0].toString(), u"anna"_s);
        QCOMPARE(signInAsked[0][1].toString(), u"a password"_s);
        QCOMPARE(signInAsked[0][2].toString(), QString());
        // A program on one's own PC stays signed in unless its box is cleared.
        QVERIFY(says(window->contentItem(), u"Keep this PC signed in for 30 days"_s));
        QCOMPARE(signInAsked[0][3].toBool(), true);
        auto* keep = window->findChild<QQuickItem*>(u"keep"_s);
        QVERIFY(keep != nullptr);
        QVERIFY(keep->setProperty("checked", false));
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(signInAsked.count(), 2);
        QCOMPARE(signInAsked[1][3].toBool(), false);

        // To a server spoken to with https nothing is said about encryption.
        shown.encrypted = true;
        session.show(shown);
        QTRY_VERIFY(!warning->isVisible());

        // Signed in: the wall, and nothing of the asking is left on the screen.
        session.show(signedIn());
        QTRY_VERIFY(!connectScreen->isVisible());
        QVERIFY(says(window->contentItem(), u"Nothing to show"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aQkvmThatWasRestartedLockedSaysSoAboveWhereSomebodySignsIn()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown;
        shown.stage = u"signIn"_s;
        shown.address = u"192.168.1.10:8787"_s;
        shown.encrypted = true;
        session.show(shown);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QString sentence =
            u"This qKVM was restarted locked, and reaches no machine until somebody has signed in."_s;
        const auto* locked = window->findChild<QQuickItem*>(u"locked"_s);
        const auto* password = window->findChild<QQuickItem*>(u"password"_s);
        QVERIFY(locked != nullptr && password != nullptr);
        // A QKVM that says nothing of the kind: nothing is said of it here.
        QVERIFY(!locked->isVisible());
        QVERIFY(!says(window->contentItem(), sentence));

        // One that was restarted locked: said above the lines that are typed into, in the colour
        // of what is told beside the matter (the dark one or the light one), not as a mistake.
        shown.locked = true;
        session.show(shown);
        QTRY_VERIFY(says(window->contentItem(), sentence));
        QVERIFY(locked->mapToScene(QPointF(0, locked->height())).y() <= password->mapToScene(QPointF(0, 0)).y());
        const QColor colour = locked->property("color").value<QColor>();
        QVERIFY2(colour == QColor(0x94, 0xa3, 0xb8) || colour == QColor(0x64, 0x74, 0x8b), qPrintable(colour.name()));
        keepPictureOf(window, u"connect-sign-in-locked"_s);
        // The keyboard is where the password is typed, as ever.
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);

        // Only where somebody signs in: not where a server is named, and not on the wall.
        shown.stage = u"address"_s;
        session.show(shown);
        QTRY_VERIFY(!locked->isVisible());
        shown = signedIn();
        shown.locked = true;
        session.show(shown);
        QTRY_VERIFY(says(window->contentItem(), u"Nothing to show"_s));
        QVERIFY(!says(window->contentItem(), sentence));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aScreenThatWaitsForItsQkvmSaysThatItIsTryingAndIsToldWhenSomebodyTypes()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown;
        shown.stage = u"address"_s;
        shown.address = u"192.168.1.10:8787"_s;
        shown.problem = u"qKVM could not be reached: Connection refused"_s;
        session.show(shown);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QString sentence =
            u"This screen opens by its display link, and asks again by itself until its qKVM answers."_s;
        const auto* trying = window->findChild<QQuickItem*>(u"tryingAgain"_s);
        QVERIFY(trying != nullptr);
        const QSignalSpy stopAsked(&session, &qkvm::ui::SessionView::stopTryingAsked);
        // A server that is not being asked again: nothing is said of it, and typing asks for
        // nothing to be stopped.
        QVERIFY(!trying->isVisible());
        QVERIFY(!says(window->contentItem(), sentence));
        type(window, u"1"_s);
        QCOMPARE(stopAsked.count(), 0);

        // One that is: said under why it was not reached, where its address is typed.
        shown.tryingAgain = true;
        session.show(shown);
        QTRY_VERIFY(says(window->contentItem(), sentence));
        QVERIFY(trying->isVisible());
        QVERIFY(says(window->contentItem(), shown.problem));
        QVERIFY(says(window->contentItem(), u"Address of your qKVM"_s));
        keepPictureOf(window, u"connect-trying-again"_s);
        // Somebody types there: whoever asks the server is told to stop.
        type(window, u"9"_s);
        QCOMPARE(stopAsked.count(), 1);
        // Told that it has stopped, the screen says it no longer; and what is typed after that
        // asks for nothing.
        shown.tryingAgain = false;
        session.show(shown);
        QTRY_VERIFY(!trying->isVisible());
        type(window, u"2"_s);
        QCOMPARE(stopAsked.count(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aCertificateIsShownToBeComparedAndNotSaidYesToByAKey()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown;
        shown.stage = u"trust"_s;
        shown.address = u"192.168.1.10:8787"_s;
        shown.encrypted = true;
        shown.certificate =
            u"00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF"_s;
        session.show(shown);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy trustAsked(&session, &qkvm::ui::SessionView::trustAsked);
        const QSignalSpy connectAsked(&session, &qkvm::ui::SessionView::connectAsked);
        const QSignalSpy signInAsked(&session, &qkvm::ui::SessionView::signInAsked);

        // Seen for the first time: the question, the certificate in four lines of eight, and where
        // the same is to be found.
        QVERIFY(says(window->contentItem(), u"Is this your qKVM?"_s));
        const QString inLines =
            u"00:11:22:33:44:55:66:77\n88:99:AA:BB:CC:DD:EE:FF\n00:11:22:33:44:55:66:77\n88:99:AA:BB:CC:DD:EE:FF"_s;
        QVERIFY(says(window->contentItem(), inLines));
        QVERIFY(says(window->contentItem(), u"Trust this certificate"_s));
        const auto* before = window->findChild<QQuickItem*>(u"certificateBefore"_s);
        QVERIFY(before != nullptr);
        QVERIFY(!before->isVisible());
        // No line to type into, and no password asked for yet.
        QVERIFY(!says(window->contentItem(), u"Password"_s));
        QVERIFY(!says(window->contentItem(), u"Address of your qKVM"_s));
        keepPictureOf(window, u"connect-trust"_s);

        // Enter, the space bar and what is typed do nothing here: a yes is a click on its button.
        QTest::keyClick(window, Qt::Key_Return);
        QTest::keyClick(window, Qt::Key_Enter);
        QTest::keyClick(window, Qt::Key_Space);
        type(window, u"yes"_s);
        QCOMPARE(trustAsked.count(), 0);
        QCOMPARE(connectAsked.count(), 0);
        QCOMPARE(signInAsked.count(), 0);
        auto* trust = window->findChild<QQuickItem*>(u"trust"_s);
        QVERIFY(trust != nullptr);
        QVERIFY(trust->isVisible());
        QVERIFY(trust->property("primary").toBool());
        QTest::mouseClick(window, Qt::LeftButton, {},
                          trust->mapToScene(QPointF(trust->width() / 2, trust->height() / 2)).toPoint());
        QCOMPARE(trustAsked.count(), 1);

        // Another certificate than before: said as that, with both, and the yes no longer the
        // button that stands out.
        shown.certificateBefore =
            u"FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00:FF:EE:DD:CC:BB:AA:99:88:77:66:55:44:33:22:11:00"_s;
        session.show(shown);
        QTRY_VERIFY(says(window->contentItem(), u"This qKVM shows another certificate than before"_s));
        QVERIFY(before->isVisible());
        QVERIFY(says(window->contentItem(), u"It shows now"_s));
        QVERIFY(says(window->contentItem(), u"It was known by"_s));
        QVERIFY(says(window->contentItem(), u"Trust the new certificate"_s));
        QVERIFY(!trust->property("primary").toBool());
        keepPictureOf(window, u"connect-trust-changed"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(trustAsked.count(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aPasswordIsNotKeptOnceItWasUsed()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown;
        shown.stage = u"signIn"_s;
        shown.address = u"192.168.1.10:8787"_s;
        session.show(shown);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy signInAsked(&session, &qkvm::ui::SessionView::signInAsked);
        const auto* password = window->findChild<QQuickItem*>(u"password"_s);
        QVERIFY(password != nullptr);

        // With one account the keyboard is in the password's line at once.
        type(window, u"a password"_s);
        QCOMPARE(password->property("text").toString(), u"a password"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(signInAsked.count(), 1);
        QCOMPARE(signInAsked[0][1].toString(), u"a password"_s);
        // A wrong one stays in its line, to be corrected.
        shown.problem = u"Wrong password"_s;
        session.show(shown);
        QCOMPARE(password->property("text").toString(), u"a password"_s);

        // In, and out again: the line is empty.
        session.show(signedIn());
        QCOMPARE(password->property("text").toString(), QString());
        session.show(shown);
        QCOMPARE(password->property("text").toString(), QString());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theAccountSaysWhoAndSignsOut()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const QSignalSpy signOutAsked(&session, &qkvm::ui::SessionView::signOutAsked);

        QObject* box = window->findChild<QObject*>(u"accountMenu"_s);
        const QQuickItem* button = theOne(window, u"accountButton"_s);
        QVERIFY(box != nullptr && button != nullptr);
        clickOn(window, button);
        QTRY_VERIFY(box->property("opened").toBool());
        // Who, as the page's drop says it, and where: the program may have more than one server.
        QCOMPARE(theOneSeen(window, u"accountWho"_s)->property("text").toString(), u"anna"_s);
        QCOMPARE(theOneSeen(window, u"accountWhere"_s)->property("text").toString(),
                 u"on the qKVM at 192.168.1.10:8787"_s);
        // With no other source to choose, no saved workspaces and no app lock, it has none of them.
        for (const QString& none : {u"sourceChoice"_s, u"browseSources"_s, u"savedWorkspaces"_s, u"lockRow"_s}) {
            QVERIFY2(seenNamed(window, none) == 0, qPrintable(none));
        }
        // What the server lets them do, under the name, in the page's words; nothing where it has
        // not said.
        QCOMPARE(seenNamed(window, u"accountRole"_s), 0);
        for (const auto& [role, said] :
             {std::pair{u"admin"_s, u"Admin"_s}, std::pair{u"operator"_s, u"Operator"_s},
              std::pair{u"viewer"_s, u"Viewer"_s}, std::pair{u"display"_s, u"A screen on the wall"_s}}) {
            qkvm::ui::SessionView::Shown shown = signedIn();
            shown.role = role;
            session.show(shown);
            QTRY_VERIFY2(theOneSeen(window, u"accountRole"_s) != nullptr, qPrintable(role));
            QCOMPARE(theOneSeen(window, u"accountRole"_s)->property("text").toString(), said);
        }
        keepPictureOf(window, u"account"_s);
        const QQuickItem* signOut = theOneSeen(window, u"signOut"_s);
        QVERIFY(signOut != nullptr);
        QVERIFY(says(signOut, u"Sign out"_s));
        clickOn(window, signOut);
        QCOMPARE(signOutAsked.count(), 1);
        QTRY_VERIFY(!box->property("opened").toBool());

        // Nobody is signed in any more: the drop goes, with the wall.
        clickOn(window, button);
        QTRY_VERIFY(box->property("opened").toBool());
        qkvm::ui::SessionView::Shown out;
        out.stage = u"signIn"_s;
        session.show(out);
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theSettingsBoxSaysWhetherTheProgramStartsWithWindowsAndChangesIt()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        qkvm::ui::PretendedStart& windows = *m_starts.back();
        const auto* box = window->findChild<QObject*>(u"settingsBox"_s);
        QVERIFY(box != nullptr);
        QVERIFY(!box->property("opened").toBool());

        // The bar's button opens it. Nothing is changed by looking. (Its General: for an admin of a
        // server's wall it opens at that wall's export and import, the first it lists.)
        clickOn(window, theOne(window, u"settingsButton"_s));
        QTRY_VERIFY(box->property("opened").toBool());
        QVERIFY(says(window->contentItem(), u"Settings"_s));
        QVERIFY(chooseSection(window, u"general"_s));
        QTRY_VERIFY(theOne(window, u"startsWithWindows"_s) != nullptr);
        const QQuickItem* tick = theOne(window, u"startsWithWindows"_s);
        QVERIFY(tick != nullptr);
        QVERIFY(says(window->contentItem(), u"Start qKVM when I sign in to Windows"_s));
        QCOMPARE(tick->property("checked").toBool(), false);
        QCOMPARE(seenNamed(window, u"settingsProblem"_s), 0);
        QVERIFY(windows.asked.isEmpty());
        keepPictureOf(window, u"settings"_s);

        // A click on the box puts the program on Windows' list.
        clickOn(window, tick);
        QTRY_COMPARE(windows.asked, QList<bool>{true});
        QCOMPARE(tick->property("checked").toBool(), true);
        QCOMPARE(seenNamed(window, u"settingsProblem"_s), 0);

        // Where Windows does not take a change, the tick shows what is so, and the box says why.
        windows.refuses = true;
        clickOn(window, tick);
        QTRY_COMPARE(windows.asked, (QList<bool>{true, false}));
        QCOMPARE(tick->property("checked").toBool(), true);
        QTRY_COMPARE(seenNamed(window, u"settingsProblem"_s), 1);
        QVERIFY(says(window->contentItem(), u"Windows did not take it: qKVM still starts with Windows."_s));
        keepPictureOf(window, u"settings-refused"_s);
        // And again: the tick is not one click behind.
        clickOn(window, tick);
        QTRY_COMPARE(windows.asked.size(), 3);
        QCOMPARE(tick->property("checked").toBool(), true);

        // Closed, and switched off in Windows' own settings meanwhile: opened again, with the
        // keyboard this time, the box shows how things are now and says nothing of before.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!box->property("opened").toBool());
        windows.refuses = false;
        windows.on = false;
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_COMPARE(theOne(window, u"startsWithWindows"_s)->property("checked").toBool(), false);
        QCOMPARE(seenNamed(window, u"settingsProblem"_s), 0);
        QCOMPARE(windows.asked.size(), 3);

        // With the keyboard: the tick is where the keyboard is first, and Space ticks it.
        QVERIFY(tabTo(window->contentItem(), u"startsWithWindows"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(windows.asked.size(), 4);
        QVERIFY(windows.on);
        QCOMPARE(theOne(window, u"startsWithWindows"_s)->property("checked").toBool(), true);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theSettingsBoxChoosesWhetherTheProgramGoesOnWhenItsWindowIsClosed()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const qkvm::core::Settings& settings = *m_settings.back();
        QVERIFY(!settings.remembered().staysInTray);

        clickOn(window, theOne(window, u"settingsButton"_s));
        QTRY_VERIFY(theOneSeen(window, u"settingsSections"_s) != nullptr);
        QVERIFY(chooseSection(window, u"general"_s));
        QTRY_VERIFY(theOne(window, u"staysInTray"_s) != nullptr);
        const QQuickItem* tick = theOne(window, u"staysInTray"_s);
        QVERIFY(says(window->contentItem(), u"Keep running in the notification area when the window is closed"_s));
        QCOMPARE(tick->property("checked").toBool(), false);
        clickOn(window, tick);
        QTRY_VERIFY(settings.remembered().staysInTray);
        QCOMPARE(tick->property("checked").toBool(), true);
        // It is no business of Windows' list.
        QVERIFY(m_starts.back()->asked.isEmpty());
        keepPictureOf(window, u"settings-stays"_s);
        // Closed and opened again, the box shows what was chosen.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->findChild<QObject*>(u"settingsBox"_s)->property("opened").toBool());
        clickOn(window, theOne(window, u"settingsButton"_s));
        QTRY_VERIFY(window->findChild<QObject*>(u"settingsBox"_s)->property("opened").toBool());
        QCOMPARE(tick->property("checked").toBool(), true);
        // So does it what was chosen while it was open, by whoever else: unticked from elsewhere.
        m_ownSettings.back()->setStaysInTray(false);
        QTRY_COMPARE(tick->property("checked").toBool(), false);
        m_ownSettings.back()->setStaysInTray(true);
        QTRY_COMPARE(tick->property("checked").toBool(), true);
        clickOn(window, tick);
        QTRY_VERIFY(!settings.remembered().staysInTray);
        QCOMPARE(tick->property("checked").toBool(), false);

        // And whether Windows shows a message of the wall while nobody sees it: until somebody
        // unticks it.
        const QQuickItem* news = theOne(window, u"tellsNews"_s);
        QVERIFY(news != nullptr);
        QVERIFY(says(window->contentItem(),
                     u"Tell me when a KVM goes away or comes back while the window is out of sight"_s));
        QVERIFY(settings.remembered().tellsNews);
        QCOMPARE(news->property("checked").toBool(), true);
        clickOn(window, news);
        QTRY_VERIFY(!settings.remembered().tellsNews);
        QCOMPARE(news->property("checked").toBool(), false);
        // The other tick is as it was: each is its own.
        QCOMPARE(tick->property("checked").toBool(), false);
        QVERIFY(!settings.remembered().staysInTray);
        // Chosen elsewhere while the box is open, it shows.
        m_ownSettings.back()->setTellsNews(true);
        QTRY_COMPARE(news->property("checked").toBool(), true);

        // The box is in the window whatever its size: the smallest there is. What its section holds
        // scrolls there, under the section's first line, and each of its controls comes into sight
        // with the keyboard: inside the window, and inside what the section shows.
        QTest::keyClick(window, Qt::Key_Escape);
        window->resize(640, 400);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(theOne(window, u"staysInTray"_s) != nullptr);
        const QQuickItem* shown = theOne(window, u"sectionContent"_s);
        QVERIFY(shown != nullptr);
        QTRY_VERIFY(isInside(window, shown));
        for (const QString& name : {u"themeChoice"_s, u"staysInTray"_s, u"tellsNews"_s, u"startsWithWindows"_s}) {
            QVERIFY2(tabTo(window->contentItem(), name), qPrintable(name));
            QTRY_VERIFY2(isInside(window, theOne(window, name)), qPrintable(name));
            const QRectF where =
                theOne(window, name)
                    ->mapRectToScene(QRectF(0, 0, theOne(window, name)->width(), theOne(window, name)->height()));
            QVERIFY2(shown->mapRectToScene(QRectF(0, 0, shown->width(), shown->height())).contains(where),
                     qPrintable(name));
        }
        keepPictureOf(window, u"settings-small"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theSettingsBoxChoosesTheTheme()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const qkvm::ui::Look& look = *m_looks.back();
        QCOMPARE(look.theme(), u"dark"_s);

        clickOn(window, theOne(window, u"settingsButton"_s));
        QTRY_VERIFY(theOneSeen(window, u"settingsSections"_s) != nullptr);
        QVERIFY(chooseSection(window, u"general"_s));
        QTRY_VERIFY(theOne(window, u"themeChoice"_s) != nullptr);
        // The one that is chosen is the one that is said.
        QVERIFY(says(theOne(window, u"themeChoice"_s), u"Dark"_s));
        clickOn(window, theOne(window, u"themeChoice"_s));
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 3);
        QVERIFY(choicesCanBeRead(window));
        keepPictureOf(window, u"settings-theme"_s);
        // Its last row overlaps the next choice, now the release key. Clicking there
        // must select a theme without opening the control beneath the popup.
        const QQuickItem* asWindows = saying(window->contentItem(), u"As Windows is set"_s);
        QVERIFY(asWindows != nullptr);
        const QQuickItem* row = asWindows->parentItem();
        const QQuickItem* choice = theOne(window, u"themeChoice"_s);
        QVERIFY(asWindows->mapToScene(QPointF(0, 0)).y() > choice->mapToScene(QPointF(0, choice->height())).y() + 40);
        const QQuickItem* nextChoice = theOne(window, u"releaseKeyChoice"_s);
        QVERIFY(nextChoice != nullptr);
        const QPointF onTheNextChoice =
            nextChoice->mapToScene(QPointF(nextChoice->width() / 2, nextChoice->height() / 2));
        QVERIFY(row->mapRectToScene(QRectF(0, 0, row->width(), row->height())).contains(onTheNextChoice));
        const QString releaseKey = m_ownSettings.back()->releaseKey();
        const bool stays = m_ownSettings.back()->staysInTray();
        const bool tells = m_ownSettings.back()->tellsNews();
        clickOn(window, onTheNextChoice.toPoint());
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QCOMPARE(look.theme(), u"system"_s);
        QCOMPARE(m_ownSettings.back()->releaseKey(), releaseKey);
        QTRY_VERIFY(says(theOne(window, u"themeChoice"_s), u"As Windows is set"_s));
        // The keyboard is back at the list's button, in the box: what lay under the list was not
        // clicked (no tick of the box's changed, nothing was asked of Windows), and nothing was
        // done to the wall.
        QTRY_VERIFY(theOne(window, u"themeChoice"_s)->hasActiveFocus());
        QCOMPARE(m_ownSettings.back()->staysInTray(), stays);
        QCOMPARE(m_ownSettings.back()->tellsNews(), tells);
        QVERIFY(m_starts.back()->asked.isEmpty());
        QCOMPARE(wall.enlarged(), QString());

        // The bar's own button goes on from there, and the box follows it.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->findChild<QObject*>(u"settingsBox"_s)->property("opened").toBool());
        clickOn(window, theOne(window, u"themeButton"_s));
        QCOMPARE(look.theme(), u"dark"_s);
        clickOn(window, theOne(window, u"settingsButton"_s));
        QTRY_VERIFY(theOne(window, u"themeChoice"_s) != nullptr);
        QVERIFY(says(theOne(window, u"themeChoice"_s), u"Dark"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runScreensTests<TestBoxScreens>(argc, argv);
}

#include "test_box_screens.moc"
