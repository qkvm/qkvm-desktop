#include "screens_test.h"

// The wall's bar and its room: the bar at every width of the window and with counts of every size,
// what moves nothing in it where something comes or goes, and what drops from it as the window
// changes size. (Its menus, and the wall: test_wall_screens.cpp.)
class TestBarScreens : public ScreensTest {
    Q_OBJECT

private slots:
    // After each test, when its windows are gone: what they were handed goes too.
    void cleanup() { forgetWhatTheWindowsWereHanded(); }

    void aWallWithoutGroupsHasNoButtonForThem()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        for (qkvm::ui::WallTile& tile : tiles) {
            tile.group.clear();
        }
        wall.machines()->setTiles(tiles);
        // A group that was chosen for another wall narrows nothing here.
        wall.shown()->setGroup(u"Rack A"_s);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const auto* button = window->findChild<QQuickItem*>(u"groupsButton"_s);
        QVERIFY(button != nullptr);
        QVERIFY(!button->isVisible());
        // Nor anything of them in More, where the window gets narrow.
        for (const int width : {1280, 800, 700, 640}) {
            window->resize(width, 600);
            QTRY_COMPARE(window->width(), width);
            letTheWallComeToRest(window);
            QVERIFY2(whatGaveWayIsInMore(window), qPrintable(QString::number(width)));
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // A filter that comes, or goes, moves nothing beside it, as nothing is to come to stand where
    // another control stood: the bar keeps the room of all four, and "Find a KVM" and the groups
    // stay where they were. (A click meant for Find landed on "Attention 1".)
    void aCountIsWrittenInTheRoomTheFiltersKeep_data()
    {
        QTest::addColumn<int>("width");
        // (The narrowest window, and each side of the widths at which something gives way while
        // the counts are short.)
        for (const int width : {640, 689, 690, 766}) {
            QTest::addRow("%d wide", width) << width;
        }
    }

    // Where the counts are written short, every count is written in the room its filter keeps:
    // on a wall of 12,000 ("12k"), a state of 9,500 was "9.5k", a sign more than the room kept for
    // the wall's own count. It is written in the wall's own way now: "9k".
    void aCountIsWrittenInTheRoomTheFiltersKeep()
    {
        QFETCH(int, width);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> many = qkvm::ui::pretendWall().mid(0, 1);
        many.reserve(12000);
        for (int at = 1; at < 12000; ++at) {
            qkvm::ui::WallTile tile = many.constFirst();
            tile.key = u"m%1"_s.arg(at);
            tile.host = u"10.0.%1.%2"_s.arg(at / 250).arg(at % 250);
            many.append(tile);
        }
        for (int at = 0; at < 12000; ++at) {
            const char* state = at < 9500 ? "online" : at < 11500 ? "no_signal" : "offline";
            many[at].state = QString::fromLatin1(state);
            many[at].group = at == 0 ? u"Rack B"_s : u"Rack A"_s;
        }
        wall.machines()->setTiles(many);
        // (One of them shown, of a group of its own: a thousand cards are a long while in the
        // making. The counts are the wall's.)
        wall.shown()->setGroup(u"Rack B"_s);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(width, 700);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_COMPARE(window->width(), width);
        letTheWallComeToRest(window);
        // (The room the filters keep for each count, as they say it.)
        const QQuickItem* filters = theOne(window, u"filter-all"_s);
        while (filters != nullptr && !filters->property("countRoom").isValid()) {
            filters = filters->parentItem();
        }
        QVERIFY(filters != nullptr);
        const qreal room = filters->property("countRoom").toReal();
        QVERIFY(room > 0);
        for (const QString& name :
             {u"filter-all"_s, u"filter-online"_s, u"filter-no_signal"_s, u"filter-attention"_s}) {
            const QQuickItem* filter = theOne(window, name);
            QVERIFY2(filter != nullptr && isSeen(filter), qPrintable(name));
            QList<const QQuickItem*> counts;
            named(filter, u"filterCount"_s, counts);
            QVERIFY2(counts.size() == 1 && counts.constFirst()->implicitWidth() <= room + 0.5,
                     qPrintable(u"%1's count is %2 wide in a room of %3"_s.arg(name)
                                    .arg(counts.isEmpty() ? -1 : counts.constFirst()->implicitWidth())
                                    .arg(room)));
            QVERIFY2(filter->implicitWidth() <= filter->property("widthShown").toReal() + 0.5,
                     qPrintable(u"%1 is %2 wide in a room of %3"_s.arg(name)
                                    .arg(filter->implicitWidth())
                                    .arg(filter->property("widthShown").toReal())));
        }
        QVERIFY(says(theOne(window, u"filter-online"_s), u"9k"_s));
        keepPictureOf(window, u"bar-counts-%1"_s.arg(width));
        // The bar's last button whole, with the bar's margin to its right, as on any wall.
        const auto* last = theOne(window, u"accountButton"_s);
        QVERIFY(last != nullptr && isSeen(last));
        QTRY_VERIFY2(
            last->mapToScene(QPointF(last->width(), 0)).x() <= width - 8,
            qPrintable(
                u"the last button ends at %1 of %2"_s.arg(last->mapToScene(QPointF(last->width(), 0)).x()).arg(width)));
        // And Find stands after the last of them, as it is laid out for the room kept.
        const QQuickItem* attention = theOne(window, u"filter-attention"_s);
        const QQuickItem* find = theOne(window, u"findAKvm"_s);
        QVERIFY(find != nullptr);
        if (isSeen(find)) {
            QVERIFY(attention->mapToScene(QPointF(attention->width(), 0)).x() <=
                    find->mapToScene(QPointF(0, 0)).x() + 0.5);
        }
        QVERIFY(whatGaveWayIsInMore(window));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aFilterThatComesOrGoesMovesNothingBesideIt()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        // Five machines, all of them online, in a group.
        QList<qkvm::ui::WallTile> tiles = fiveMachines();
        for (qkvm::ui::WallTile& tile : tiles) {
            tile.group = u"Rack A"_s;
        }
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QQuickItem* find = theOne(window, u"findAKvm"_s);
        const QQuickItem* groups = theOne(window, u"groupsButton"_s);
        QVERIFY(find != nullptr && groups != nullptr && theOne(window, u"filter-all"_s) != nullptr);
        QTRY_VERIFY(isSeen(groups));
        QVERIFY(!isSeen(theOne(window, u"filter-attention"_s)));
        letTheWallComeToRest(window);
        const auto places = [&] {
            // (The filters' buttons are made anew as the counts change.)
            const QQuickItem* all = theOne(window, u"filter-all"_s);
            return QList<qreal>{all == nullptr ? -1 : all->mapToScene(QPointF(0, 0)).x(),
                                find->mapToScene(QPointF(0, 0)).x(), groups->mapToScene(QPointF(0, 0)).x()};
        };
        const QList<qreal> before = places();

        // One that wants attention, and then one with no signal: their filters come, and nothing
        // beside them moves.
        tiles[2].state = u"offline"_s;
        tiles[2].label = u"Offline"_s;
        wall.machines()->setTiles(tiles);
        QTRY_VERIFY(isSeen(theOne(window, u"filter-attention"_s)));
        letTheWallComeToRest(window);
        QCOMPARE(places(), before);
        tiles[1].state = u"no_signal"_s;
        tiles[1].label = u"No signal"_s;
        wall.machines()->setTiles(tiles);
        QTRY_VERIFY(isSeen(theOne(window, u"filter-no_signal"_s)));
        letTheWallComeToRest(window);
        QCOMPARE(places(), before);
        keepPictureOf(window, u"filters-all-four"_s);
        // And they go again.
        for (qkvm::ui::WallTile& tile : tiles) {
            tile.state = u"online"_s;
            tile.label = u"Online"_s;
        }
        wall.machines()->setTiles(tiles);
        QTRY_VERIFY(!isSeen(theOne(window, u"filter-attention"_s)));
        letTheWallComeToRest(window);
        QCOMPARE(places(), before);

        // Twelve machines, nine online and three that want attention; then a tenth online: a count
        // with a figure more moves nothing either, where the wall has as many figures.
        QList<qkvm::ui::WallTile> twelve;
        for (int at = 0; at < 12; ++at) {
            qkvm::ui::WallTile tile = tiles.constFirst();
            tile.key = u"m%1"_s.arg(at);
            tile.name = u"Machine %1"_s.arg(at);
            tile.host = u"192.168.1.%1"_s.arg(100 + at);
            tile.state = at < 9 ? u"online"_s : u"offline"_s;
            tile.label = at < 9 ? u"Online"_s : u"Offline"_s;
            twelve.append(tile);
        }
        wall.machines()->setTiles(twelve);
        QTRY_VERIFY(isSeen(theOne(window, u"filter-attention"_s)));
        letTheWallComeToRest(window);
        const QList<qreal> ofTwelve = places();
        twelve[9].state = u"online"_s;
        twelve[9].label = u"Online"_s;
        wall.machines()->setTiles(twelve);
        QTRY_COMPARE(wall.machines()->online(), 10);
        letTheWallComeToRest(window);
        QCOMPARE(places(), ofTwelve);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBarStaysInsideTheWindowAtEveryWidth_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<int>("machines");
        // The narrowest window there is, every width at which the bar lets something go, the one
        // below each, and two wide ones: on the wall of fifteen the widths are measured on, and,
        // with all the bar ever says, on walls of a hundred and of a thousand machines, whose
        // counts the filters keep more room for.
        for (const int width : {640,  689,  690,  766,  767,  809,  810,  959,  960,  1001, 1002, 1091,
                                1092, 1165, 1166, 1215, 1216, 1269, 1270, 1280, 1399, 1400, 1920}) {
            QTest::addRow("%d wide", width) << width << true << 15;
            QTest::addRow("%d wide, and not encrypted", width) << width << false << 15;
            QTest::addRow("%d wide, not encrypted, 100 machines", width) << width << false << 100;
            QTest::addRow("%d wide, not encrypted, 1000 machines", width) << width << false << 1000;
        }
        // And ten thousand, where the bar is narrow: the counts are written short there ("10k").
        for (const int width : {640, 689, 690, 766, 767, 809, 810}) {
            QTest::addRow("%d wide, not encrypted, 10000 machines", width) << width << false << 10000;
        }
    }

    void theBarStaysInsideTheWindowAtEveryWidth()
    {
        QFETCH(int, width);
        QFETCH(bool, encrypted);
        QFETCH(int, machines);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        if (machines == 15) {
            wall.machines()->setTiles(qkvm::ui::pretendWall());
        } else {
            // As many, in a group (the bar has the groups' button then); and none of them shown,
            // with "No signal" chosen, which none has: the filters keep the same room for all four
            // whichever are shown, and a thousand cards are a long while in the making.
            QList<qkvm::ui::WallTile> many = qkvm::ui::pretendWall().mid(0, 1);
            many.reserve(machines);
            for (int at = 1; at < machines; ++at) {
                qkvm::ui::WallTile tile = many.constFirst();
                tile.key = u"m%1"_s.arg(at);
                tile.host = u"10.0.%1.%2"_s.arg(at / 250).arg(at % 250);
                many.append(tile);
            }
            wall.machines()->setTiles(many);
            wall.shown()->setShow(u"no_signal"_s);
        }
        // (With all the bar ever says at once: where nothing is encrypted, the pictures are paused too.)
        wall.setPace(encrypted ? 1000 : 0);
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown = signedIn();
        shown.encrypted = encrypted;
        session.show(shown);
        // Whoever is signed in may act and manages: every button of the bar is there.
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(theOne(window, u"selectButton"_s) != nullptr);
        window->resize(width, 700);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_COMPARE(window->width(), width);

        // The last button of the bar, whole, with the bar's margin to its right; and nothing of the
        // bar before the window's left edge.
        const auto* last = window->findChild<QQuickItem*>(u"accountButton"_s);
        QVERIFY(last != nullptr);
        QVERIFY(last->isVisible());
        QTRY_VERIFY2(
            last->mapToScene(QPointF(last->width(), 0)).x() <= width - 8,
            qPrintable(
                u"the last button ends at %1 of %2"_s.arg(last->mapToScene(QPointF(last->width(), 0)).x()).arg(width)));
        // Where it is so, that nothing is encrypted is said at every width.
        const auto* notEncrypted = window->findChild<QQuickItem*>(u"barNotEncrypted"_s);
        QVERIFY(notEncrypted != nullptr);
        QCOMPARE(notEncrypted->isVisible(), !encrypted);
        if (!encrypted) {
            QVERIFY(notEncrypted->mapToScene(QPointF(0, 0)).x() >= 0);
            QVERIFY(notEncrypted->width() > 0);
            keepPictureOf(window, machines == 15 ? u"bar-%1"_s.arg(width) : u"bar-%1-of-%2"_s.arg(width).arg(machines));
        }
        QVERIFY(whatGaveWayIsInMore(window));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // What drops from the bar goes with it: where a machine is shown large (also one a notice of
    // Windows' was about), where the wall is only shown, and where nobody is signed in any more; it
    // would stand alone over what came, and hold the keys (which are the wall's again once the
    // wall is). And where the window changes size, or the rows of a menu are made anew, it closes:
    // its rows and its height were for the window as it was.
    void whatDropsFromTheBarGoesWithItAndWithTheWindowsSize()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(1280, 800);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), 1280);
        const auto opened = [&](const QString& menu) {
            const QObject* one = window->findChild<QObject*>(menu);
            return one != nullptr && one->property("opened").toBool();
        };
        const auto openBy = [&](const QString& button, const QString& menu) {
            const QQuickItem* one = theOne(window, button);
            if (one == nullptr || !isSeen(one)) {
                return false;
            }
            clickOn(window, one);
            return QTest::qWaitFor([&] { return opened(menu); }, 2000);
        };

        for (const auto& [button, menu] :
             {std::pair{u"viewButton"_s, u"viewMenu"_s}, std::pair{u"groupsButton"_s, u"groupsMenu"_s},
              std::pair{u"accountButton"_s, u"accountMenu"_s}}) {
            QVERIFY2(openBy(button, menu), qPrintable(menu));
            wall.enlarge(tiles[1].key);
            QTRY_VERIFY2(!opened(menu), qPrintable(menu));
            wall.closeEnlarged();
            QTRY_VERIFY(isSeen(theOne(window, button)));
        }
        // (A notice's machine: as soon as it is asked to be shown.)
        QVERIFY(openBy(u"viewButton"_s, u"viewMenu"_s));
        QVERIFY(QMetaObject::invokeMethod(&wall, "revealAsked"));
        QTRY_VERIFY(!opened(u"viewMenu"_s));
        QVERIFY(openBy(u"viewButton"_s, u"viewMenu"_s));
        wall.setDisplaying(true);
        QTRY_VERIFY(!opened(u"viewMenu"_s));
        wall.setDisplaying(false);
        QTRY_VERIFY(isSeen(theOne(window, u"viewButton"_s)));
        QVERIFY(openBy(u"viewButton"_s, u"viewMenu"_s));
        qkvm::ui::SessionView::Shown nobody;
        nobody.stage = u"signIn"_s;
        session.show(nobody);
        QTRY_VERIFY(!opened(u"viewMenu"_s));
        session.show(signedIn());
        QTRY_VERIFY(isSeen(theOne(window, u"viewButton"_s)));
        letTheWallComeToRest(window);
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(wall.selecting());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!wall.selecting());

        // (Its width alone, and its height alone.)
        QVERIFY(openBy(u"viewButton"_s, u"viewMenu"_s));
        window->resize(1200, 800);
        QTRY_VERIFY(!opened(u"viewMenu"_s));
        QVERIFY(openBy(u"viewButton"_s, u"viewMenu"_s));
        window->resize(1200, 700);
        QTRY_VERIFY(!opened(u"viewMenu"_s));
        window->resize(700, 600);
        QTRY_COMPARE(window->size(), QSize(700, 600));
        letTheWallComeToRest(window);
        QVERIFY(openBy(u"moreButton"_s, u"moreMenu"_s));
        window->resize(900, 600);
        QTRY_VERIFY(!opened(u"moreMenu"_s));
        window->resize(1280, 800);
        QTRY_COMPARE(window->size(), QSize(1280, 800));
        letTheWallComeToRest(window);
        QVERIFY(openBy(u"groupsButton"_s, u"groupsMenu"_s));
        tiles[0].group = u"Cellar"_s;
        wall.machines()->setTiles(tiles);
        QTRY_VERIFY(!opened(u"groupsMenu"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Where the window is too narrow for the groups' button, More keeps its place on a wall that
    // has no groups, where nothing else gave way: the wall's first group to come moves nothing.
    void aWallsFirstGroupMovesNothingInTheBar()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        for (qkvm::ui::WallTile& tile : tiles) {
            tile.group.clear();
        }
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(790, 600);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), 790);
        letTheWallComeToRest(window);
        QVERIFY(!isSeen(theOne(window, u"moreButton"_s)));
        QVERIFY(isSeen(theOne(window, u"moreSlot"_s)));
        QVERIFY(whatGaveWayIsInMore(window));
        const auto at = [&](const QString& name) { return theOne(window, name)->mapToScene(QPointF(0, 0)).x(); };
        const QStringList buttons{u"viewButton"_s, u"selectButton"_s, u"settingsButton"_s, u"accountButton"_s};
        QList<qreal> before;
        for (const QString& name : buttons) {
            before.append(at(name));
        }
        tiles[0].group = u"Rack A"_s;
        wall.machines()->setTiles(tiles);
        QTRY_VERIFY(isSeen(theOne(window, u"moreButton"_s)));
        letTheWallComeToRest(window);
        QVERIFY(!isSeen(theOne(window, u"moreSlot"_s)));
        for (int index = 0; index < buttons.size(); ++index) {
            QCOMPARE(at(buttons[index]), before[index]);
        }
        QVERIFY(whatGaveWayIsInMore(window));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runScreensTests<TestBarScreens>(argc, argv);
}

#include "test_bar_screens.moc"
