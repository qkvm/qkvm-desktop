#include <algorithm>

#include <QScopeGuard>

#include "screens_test.h"

// The wall and its cards: the bar's menus, the cards and what lies on them, a click that shows a
// machine large and the way back, picking, ordering, display mode, what lies over the wall, and
// the windows of the wall. (The bar's room at every width: test_bar_screens.cpp.)
class TestWallScreens : public ScreensTest {
    Q_OBJECT

private slots:
    // After each test, when its windows are gone: what they were handed goes too.
    void cleanup() { forgetWhatTheWindowsWereHanded(); }

    void theWallComesUpWithACardForEveryMachine()
    {
        qkvm::ui::AboutModel about;
        about.useDiagnostics([] { return true; });
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        // The window does not show itself: whoever makes it gives it the engine first.
        QVERIFY(!window->isVisible());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(window->title(), u"qKVM"_s);

        // A place for a picture in every card, fifteen of them; the engine is told of nine.
        QCOMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        keepPictureOf(window, u"wall"_s);

        // What the bar narrows the wall to is what is left of the cards.
        wall.shown()->setShow(u"attention"_s);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 2);
        keepPictureOf(window, u"wall-attention"_s);
        wall.shown()->setShow(u"all"_s);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);

        // The screen says how wide a card's picture is drawn: pictures are asked of the server by that.
        QTRY_VERIFY(wall.cardPixels() > 100);

        QObject* box = window->findChild<QObject*>(u"aboutBox"_s);
        QVERIFY(box != nullptr);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        keepPictureOf(window, u"about"_s);

        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aCardCoversItsPictureOnlyWhereItHasSomethingToSay()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        // A KVM that is live, a server that runs with a fault and has its own screen on its card,
        // and a KVM that cannot be reached.
        qkvm::ui::WallTile live;
        live.key = u"a"_s;
        live.name = u"Build server"_s;
        live.state = u"online"_s;
        live.stream = 1;
        qkvm::ui::WallTile faulty;
        faulty.key = u"s"_s;
        faulty.name = u"Storage server"_s;
        faulty.state = u"error"_s;
        faulty.stream = 2;
        faulty.meta = u"1024×768 · Critical"_s;
        qkvm::ui::WallTile gone;
        gone.key = u"b"_s;
        gone.name = u"Old server"_s;
        gone.state = u"offline"_s;
        gone.title = u"Offline"_s;
        gone.text = u"Device is unreachable"_s;
        wall.machines()->setTiles({live, faulty, gone});
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 3);
        int covered = 0;
        QList<const QQuickItem*> veils;
        named(window->contentItem(), u"veil"_s, veils);
        QCOMPARE(veils.size(), 3);
        for (const QQuickItem* veil : std::as_const(veils)) {
            covered += veil->isVisible() ? 1 : 0;
        }
        QCOMPARE(covered, 1);
        QVERIFY(says(window->contentItem(), u"Offline"_s));
        keepPictureOf(window, u"wall-server-screen"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theWallSaysWhichCardsAreInSight()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        const auto key = [&](int row) {
            return wall.machines()->index(row, 0).data(qkvm::ui::WallModel::KeyRole).toString();
        };

        // Too many machines for a window this small: readable cards, and scrolling. The first cards
        // are in sight, with the row after them, and the last are not.
        window->resize(700, 420);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(!wall.inSight().isEmpty() && wall.inSight().size() < 15);
        QVERIFY(wall.inSight().contains(key(0)));
        QVERIFY(!wall.inSight().contains(key(14)));
        const qsizetype atTheTop = wall.inSight().size();

        // Scrolled to the end, it is the other way round.
        auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        QVERIFY(scrolled->property("contentHeight").toReal() > scrolled->height());
        QVERIFY(scrolled->setProperty("contentY", scrolled->property("contentHeight").toReal() - scrolled->height()));
        QTRY_VERIFY(wall.inSight().contains(key(14)));
        QVERIFY(!wall.inSight().contains(key(0)));
        QVERIFY(wall.inSight().size() <= atTheTop + 2);

        // In a window that holds them all, all are in sight.
        window->resize(1600, 1000);
        QTRY_COMPARE(wall.inSight().size(), 15);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aClickShowsAMachineLargeAndEscGoesBack()
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
        const auto key = [&](int row) {
            return wall.machines()->index(row, 0).data(qkvm::ui::WallModel::KeyRole).toString();
        };
        const auto name = [&](int row) {
            return wall.machines()->index(row, 0).data(qkvm::ui::WallModel::NameRole).toString();
        };
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);

        // A click on a card shows its machine large, as on the page; the pointer says so over it.
        // (A click put the keyboard on a card and enlarged nothing; a double click enlarged it.)
        QList<const QQuickItem*> veils;
        const QQuickItem* second = nullptr;
        const std::function<void(const QQuickItem*)> find = [&](const QQuickItem* under) {
            if (under->inherits("QQuickText") && under->property("text").toString() == name(1) && under->isVisible()) {
                second = under;
            }
            const QList<QQuickItem*> children = under->childItems();
            for (const QQuickItem* child : children) {
                find(child);
            }
        };
        find(window->contentItem());
        QVERIFY(second != nullptr);
        // The name stands in the line under the picture: the picture is just above it.
        const QPoint onTheCard = second->mapToScene(QPointF(4, -40)).toPoint();
        const auto* opens = inCard(cardOf(window->contentItem(), key(1)), u"cardOpens"_s);
        QVERIFY(opens != nullptr);
        QCOMPARE(opens->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseClick(window, Qt::LeftButton, {}, onTheCard);
        QTRY_COMPARE(wall.enlarged(), key(1));

        // In the wall's place: one picture's place to be seen and no more, its name in the bar, and
        // not the wall's bar.
        QTRY_COMPARE(seen<qkvm::ui::VideoTile>(window->contentItem()), 1);
        QVERIFY(says(window->contentItem(), name(1)));
        const auto* state = theOne(window, u"consoleMachineState"_s);
        QVERIFY(state != nullptr && state->isVisible());
        QCOMPARE(state->property("color").value<QColor>(), QColor(u"#22c55e"_s));
        const auto* account = window->findChild<QQuickItem*>(u"accountButton"_s);
        QVERIFY(account != nullptr);
        QVERIFY(!account->isVisible());
        // The screen has said how wide the picture is drawn: the whole window.
        QTRY_VERIFY(wall.enlargedPixels() >= 1200);
        keepPictureOf(window, u"enlarged"_s);

        // The arrow keys go to the machines beside it, Esc back to the wall.
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(wall.enlarged(), key(2));
        QVERIFY(says(window->contentItem(), name(2)));
        QTest::keyClick(window, Qt::Key_Left);
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(wall.enlarged(), key(0));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        // The wall again: the pictures of the nine machines that have one.
        QTRY_COMPARE(seen<qkvm::ui::VideoTile>(window->contentItem()), qkvm::ui::pretendWallPictured());
        // And the place for the large picture is gone with it, not kept out of sight.
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        QVERIFY(account->isVisible());
        // A double click shows it large as well, and leaves it so.
        letTheWallComeToRest(window);
        QTest::mouseDClick(window, Qt::LeftButton, {}, onTheCard);
        QTRY_COMPARE(wall.enlarged(), key(1));
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
        QCOMPARE(wall.enlarged(), key(1));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());

        // A machine without a picture is shown large as it is shown small: with what it says.
        int offline = -1;
        for (int row = 0; row < wall.machines()->rowCount() && offline < 0; ++row) {
            offline = wall.machines()->index(row, 0).data(qkvm::ui::WallModel::StateRole).toString() == u"offline"_s
                          ? row
                          : -1;
        }
        QVERIFY(offline >= 0);
        wall.enlarge(key(offline));
        QTRY_VERIFY(says(window->contentItem(), name(offline)));
        QVERIFY(says(window->contentItem(), u"Offline"_s));
        keepPictureOf(window, u"enlarged-offline"_s);
        // The buttons do what the keys do.
        auto* close = window->findChild<QQuickItem*>(u"enlargedClose"_s);
        QVERIFY(close != nullptr);
        QTest::mouseClick(window, Qt::LeftButton, {},
                          close->mapToScene(QPointF(close->width() / 2, close->height() / 2)).toPoint());
        QTRY_COMPARE(wall.enlarged(), QString());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // What lies on a card takes its own click, and that is all the click does: the machine is not
    // shown large by it, as on the page (app.js: a click on a tile's link or button is not a click
    // on the tile). The card's star, the ways into its console and into its menu, the numbers of
    // the computers behind it, and the button of what it says.
    void aClickOnWhatLiesOnACardIsThatThingsAlone()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        wall.setFavorites({}, true);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QSignalSpy favorite(&wall, &qkvm::ui::WallView::favoriteAsked);
        const QSignalSpy console(&actions, &qkvm::ui::ActionsView::consoleAsked);
        const QSignalSpy menu(&actions, &qkvm::ui::ActionsView::openAsked);
        const QSignalSpy port(&actions, &qkvm::ui::ActionsView::portAsked);
        const QSignalSpy cardAction(&actions, &qkvm::ui::ActionsView::cardActionAsked);
        const auto nothingIsShownLarge = [&] {
            QTest::qWait(100);
            return wall.enlarged().isEmpty();
        };

        // The pointer on the first card: its star, and the ways into its console and its menu.
        const QQuickItem* first = cardOf(window->contentItem(), tiles[0].key);
        QVERIFY(first != nullptr);
        QTest::mouseMove(window, first->mapToScene(QPointF(first->width() / 2, first->height() / 2)).toPoint());
        for (const QString& name : {u"cardFavorite"_s, u"cardConsole"_s, u"cardActions"_s}) {
            QTRY_VERIFY2(inCard(first, name) != nullptr && isSeen(inCard(first, name)), qPrintable(name));
        }
        clickOn(window, inCard(first, u"cardFavorite"_s));
        QTRY_COMPARE(favorite.count(), 1);
        QVERIFY(nothingIsShownLarge());
        clickOn(window, inCard(first, u"cardConsole"_s));
        QTRY_COMPARE(console.count(), 1);
        QVERIFY(nothingIsShownLarge());
        clickOn(window, inCard(first, u"cardActions"_s));
        QTRY_COMPARE(menu.count(), 1);
        QVERIFY(nothingIsShownLarge());

        // A number of one of the computers behind a KVM.
        const auto several =
            std::ranges::find_if(tiles, [](const qkvm::ui::WallTile& tile) { return !tile.ports.isEmpty(); });
        QVERIFY(several != tiles.end());
        QList<const QQuickItem*> ports;
        named(cardOf(window->contentItem(), several->key), u"port"_s, ports);
        QVERIFY(ports.size() >= 3);
        clickOn(window, ports[2]);
        QTRY_COMPARE(port.count(), 1);
        QVERIFY(nothingIsShownLarge());

        // The button of what a card says, at the end of the wall.
        auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        QVERIFY(scrolled->setProperty(
            "contentY", std::max(0.0, scrolled->property("contentHeight").toReal() - scrolled->height())));
        const auto refused = std::ranges::find_if(
            tiles, [](const qkvm::ui::WallTile& tile) { return tile.action == u"Update login"_s; });
        QVERIFY(refused != tiles.end());
        const QQuickItem* update = inCard(cardOf(window->contentItem(), refused->key), u"cardAction"_s);
        QVERIFY(update != nullptr);
        QTRY_VERIFY(isInside(window, update));
        clickOn(window, update);
        QTRY_COMPARE(cardAction.count(), 1);
        QVERIFY(nothingIsShownLarge());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Every machine of a wall is shown large without a word of complaint: one click opens any card
    // now, a server's too. (A server's readout, made while the large view was still being laid
    // out, asked for fewer than no lines: Qt said so, and the boxes of a file found it.)
    void everyMachineIsShownLargeWithoutAWordOfComplaint()
    {
        QTest::failOnWarning(QRegularExpression(u"Model size of .* is less than 0"_s));
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        for (const qkvm::ui::WallTile& tile : tiles) {
            const QQuickItem* card = cardOf(window->contentItem(), tile.key);
            QVERIFY(card != nullptr);
            auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
            QVERIFY(scrolled->setProperty(
                "contentY",
                std::clamp(card->y() - 20, 0.0,
                           std::max(0.0, scrolled->property("contentHeight").toReal() - scrolled->height()))));
            letTheWallComeToRest(window);
            clickOn(window, card->mapToScene(QPointF(card->width() / 2, 40)).toPoint());
            QTRY_COMPARE(wall.enlarged(), tile.key);
            letTheWallComeToRest(window);
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_COMPARE(wall.enlarged(), QString());
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // A double click on a card (which is how a card was opened before one click did, and how a
    // hand used to Windows may still open it) shows the machine large with its first click. Its
    // second lands where the card was, where the machine shown large has its buttons now: it is no
    // click on them. A moment later a click there is one again; and so it is at once where the
    // machine shown large is another than the click showed.
    void theSecondClickOfADoubleClickThatShowedAMachineLargeIsNoClickOnWhatCameThere()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        // Favourites are kept: a machine shown large has a button for one, in its second row.
        wall.setFavorites({}, true);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        // (Longer than a double click takes: the test's second click comes within it, however
        // slowly the window is drawn.)
        QVERIFY(window->setProperty("secondClickMs", 3000));
        const QSignalSpy favorite(&wall, &qkvm::ui::WallView::favoriteAsked);

        // Where a machine shown large has that button, and the card that lies there on the wall.
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedFavorite"_s) != nullptr && isSeen(theOne(window, u"enlargedFavorite"_s)));
        letTheWallComeToRest(window);
        const QQuickItem* star = theOne(window, u"enlargedFavorite"_s);
        const QRectF starIn = star->mapRectToScene(QRectF(0, 0, star->width(), star->height()));
        wall.closeEnlarged();
        QTRY_VERIFY(window->findChild<QQuickItem*>(u"wall"_s)->isVisible());
        letTheWallComeToRest(window);
        // (Over a card, and not over the gap between two.)
        QPoint onTheStar;
        const QQuickItem* card = nullptr;
        for (const qkvm::ui::WallTile& tile : tiles) {
            const QQuickItem* one = cardOf(window->contentItem(), tile.key);
            const QRectF both = one->mapRectToScene(QRectF(0, 0, one->width(), one->height())).intersected(starIn);
            if (card == nullptr && both.width() >= 6) {
                card = one;
                onTheStar = QPointF(both.center().x(), starIn.center().y()).toPoint();
            }
        }
        QVERIFY2(card != nullptr, "no card lies where a machine shown large has its star");
        QCOMPARE(cardAt(window->contentItem(), onTheStar), card);
        const QString key = card->property("key").toString();
        for (const QString& own : {u"cardFavorite"_s, u"cardConsole"_s, u"cardActions"_s}) {
            const QQuickItem* button = inCard(card, own);
            QVERIFY(button != nullptr);
            QVERIFY2(!button->mapRectToScene(QRectF(0, 0, button->width(), button->height())).contains(onTheStar),
                     qPrintable(own));
        }

        const auto starIsThere = [&] {
            const QQuickItem* shown = theOne(window, u"enlargedFavorite"_s);
            return shown != nullptr && isSeen(shown) && isSeenAt(shown, onTheStar);
        };

        // A click there shows that machine large; a second one, a moment later, does nothing.
        clickOn(window, onTheStar);
        QTRY_COMPARE(wall.enlarged(), key);
        QTRY_VERIFY(starIsThere());
        clickOn(window, onTheStar);
        QTest::qWait(100);
        QCOMPARE(favorite.count(), 0);
        QCOMPARE(wall.enlarged(), key);
        // Once a double click is over, a click there is a click on the star.
        const QQuickItem* held = theOne(window, u"secondClickOfAnOpening"_s);
        QVERIFY(held != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!held->property("enabled").toBool(), 10000);
        clickOn(window, onTheStar);
        QTRY_COMPARE(favorite.count(), 1);

        // Shown large by a click, back to the wall, and shown large again by the program: a click
        // there is a click on the star at once.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        letTheWallComeToRest(window);
        clickOn(window, onTheStar);
        QTRY_COMPARE(wall.enlarged(), key);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        wall.enlarge(key);
        QTRY_VERIFY(starIsThere());
        clickOn(window, onTheStar);
        QTRY_COMPARE(favorite.count(), 2);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Back from a machine shown large, by Esc or the cross, the keyboard is on the card of the
    // machine that was shown last, and that card is in sight, as on the page (app.js,
    // closeFocus): Enter, or Space, shows it again.
    void backFromAMachineShownLargeTheKeyboardIsOnItsCard()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);

        // A click on the second card, and Esc: the keyboard is on that card, and Enter shows it.
        const QQuickItem* second = cardOf(window->contentItem(), tiles[1].key);
        QVERIFY(second != nullptr);
        clickOn(window, second->mapToScene(QPointF(second->width() / 2, 60)).toPoint());
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        QTRY_VERIFY(second->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);

        // A step, and then a machine whose card is out of sight on the wall, shown by the program:
        // back, the keyboard is on its card, and the wall has scrolled it into sight.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        const QRectF inSight = scrolled->mapRectToScene(QRectF(0, 0, scrolled->width(), scrolled->height()));
        const QQuickItem* last = nullptr;
        for (const qkvm::ui::WallTile& tile : tiles) {
            const QQuickItem* card = cardOf(window->contentItem(), tile.key);
            if (last == nullptr &&
                !inSight.contains(card->mapRectToScene(QRectF(0, 0, card->width(), card->height())))) {
                last = card;
            }
        }
        QVERIFY2(last != nullptr, "every card is in sight");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(wall.enlarged(), tiles[2].key);
        wall.enlarge(last->property("key").toString());
        QTRY_COMPARE(wall.enlarged(), last->property("key").toString());
        clickOn(window, theOne(window, u"enlargedClose"_s));
        QTRY_COMPARE(wall.enlarged(), QString());
        QTRY_VERIFY(last->hasActiveFocus());
        QTRY_VERIFY(scrolled->mapRectToScene(QRectF(0, 0, scrolled->width(), scrolled->height()))
                        .contains(last->mapRectToScene(QRectF(0, 0, last->width(), last->height()))));
        // Space shows it, as Enter does (app.js, the tile's keydown).
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(wall.enlarged(), last->property("key").toString());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(last->hasActiveFocus());

        // Shown large, and signed out meanwhile: once none is shown, the keyboard stays where the
        // next thing is typed, and no card out of sight takes it.
        wall.enlarge(tiles[1].key);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        qkvm::ui::SessionView::Shown out = signedIn();
        out.stage = u"signIn"_s;
        session.show(out);
        QTRY_VERIFY(window->activeFocusItem() != nullptr && window->activeFocusItem()->inherits("QQuickTextInput"));
        const QQuickItem* typedIn = window->activeFocusItem();
        wall.closeEnlarged();
        QTest::qWait(100);
        QCOMPARE(window->activeFocusItem(), typedIn);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // A machine shown large leaves the wall while a box lies over the window (a KVM taken off in
    // the settings, or by its server): the box keeps the keyboard, and its Esc still closes it;
    // and once it is gone, the wall has the keyboard (the machine's card went with it).
    void aMachineThatLeavesTheWallUnderABoxLeavesTheKeyboardInTheBox()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        QObject* settings = window->findChild<QObject*>(u"settingsBox"_s);
        QVERIFY(settings != nullptr);
        const auto inTheBox = [&] {
            const QQuickItem* box = settings->property("contentItem").value<QQuickItem*>()->parentItem();
            for (const QQuickItem* up = window->activeFocusItem(); up != nullptr; up = up->parentItem()) {
                if (up == box) {
                    return true;
                }
            }
            return false;
        };

        // The settings open and settled over a machine shown large, and that machine leaves the
        // wall, with no other box going meanwhile: the keyboard is in the box at once, and after.
        wall.enlarge(tiles[1].key);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTRY_VERIFY(inTheBox());
        letTheWallComeToRest(window);
        tiles.removeAt(1);
        wall.machines()->setTiles(tiles);
        QTRY_COMPARE(wall.enlarged(), QString());
        QVERIFY(inTheBox());
        QTest::qWait(100);
        QVERIFY(inTheBox());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());
        QTRY_VERIFY(window->findChild<QQuickItem*>(u"wall"_s)->hasActiveFocus());

        // Again, and the machine leaves the wall as the box asked (a box over the box, said yes to
        // and closed in the same turn), and nothing is shown large any more.
        wall.enlarge(tiles[1].key);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTRY_VERIFY(inTheBox());
        QObject* asked = window->findChild<QObject*>(u"aboutBox"_s);
        QVERIFY(QMetaObject::invokeMethod(asked, "open"));
        QTRY_VERIFY(asked->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(asked, "close"));
        tiles.removeAt(1);
        wall.machines()->setTiles(tiles);
        QTRY_COMPARE(wall.enlarged(), QString());
        QTRY_VERIFY(!asked->property("opened").toBool());
        QTest::qWait(100);
        QVERIFY(inTheBox());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());
        QTRY_VERIFY(window->findChild<QQuickItem*>(u"wall"_s)->hasActiveFocus());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Where a box lies over the window as the wall is to take the keyboard (at sign-in, as display
    // mode begins), the box keeps it, and the wall takes it once the box is gone. And what is owed
    // so is forgotten where the box goes while the wall is out of sight: signed in again later,
    // "Find a KVM" keeps the keyboard when a box opens and closes over it.
    void aBoxThatLiesOverTheWindowKeepsTheKeyboardUntilItGoes()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        QObject* aboutBox = window->findChild<QObject*>(u"aboutBox"_s);
        QObject* settings = window->findChild<QObject*>(u"settingsBox"_s);
        QVERIFY(aboutBox != nullptr && settings != nullptr);
        const auto inThe = [&](const QObject* box) {
            const QQuickItem* item = box->property("contentItem").value<QQuickItem*>()->parentItem();
            for (const QQuickItem* up = window->activeFocusItem(); up != nullptr; up = up->parentItem()) {
                if (up == item) {
                    return true;
                }
            }
            return false;
        };

        // Signed in while a box lies over the window that asks which qKVM.
        QVERIFY(QMetaObject::invokeMethod(aboutBox, "open"));
        QTRY_VERIFY(aboutBox->property("opened").toBool());
        QTRY_VERIFY(inThe(aboutBox));
        session.show(signedIn());
        QTRY_VERIFY(scrolled->isVisible());
        QTest::qWait(100);
        QVERIFY(inThe(aboutBox));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!aboutBox->property("opened").toBool());
        QTRY_VERIFY(scrolled->hasActiveFocus());

        // Display mode begins under a box (as for a display link, or a wall screen).
        QVERIFY(QMetaObject::invokeMethod(aboutBox, "open"));
        QTRY_VERIFY(aboutBox->property("opened").toBool());
        QTRY_VERIFY(inThe(aboutBox));
        wall.setDisplaying(true);
        QTest::qWait(100);
        QVERIFY(inThe(aboutBox));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!aboutBox->property("opened").toBool());
        QTRY_VERIFY(scrolled->hasActiveFocus());
        wall.setDisplaying(false);

        // A machine shown large leaves the wall under the settings: owed. Signed out, and the box
        // goes then: nothing is owed any more.
        wall.enlarge(tiles[1].key);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        tiles.removeAt(1);
        wall.machines()->setTiles(tiles);
        QTRY_COMPARE(wall.enlarged(), QString());
        QTRY_COMPARE(scrolled->property("owedTo").toString(), u"card"_s);
        qkvm::ui::SessionView::Shown out = signedIn();
        out.stage = u"signIn"_s;
        session.show(out);
        QTRY_VERIFY(!scrolled->isVisible());
        QVERIFY(QMetaObject::invokeMethod(settings, "close"));
        QTRY_VERIFY(!settings->property("opened").toBool());
        QTRY_COMPARE(scrolled->property("owedTo").toString(), QString());
        // In again; "/" into Find, a box opened over it and closed: Find keeps the keyboard.
        session.show(signedIn());
        QTRY_VERIFY(scrolled->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Slash);
        const QQuickItem* find = theOne(window, u"findAKvm"_s);
        QTRY_VERIFY(find->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());
        QTRY_VERIFY(find->hasActiveFocus());
        QTest::qWait(150);
        QVERIFY(find->hasActiveFocus());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void enterOnACardShowsItLarge()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        // Tab walks the bar's buttons and then the cards; Enter on a card enlarges that one.
        for (int presses = 0; presses < 40 && wall.enlarged().isEmpty(); ++presses) {
            QTest::keyClick(window, Qt::Key_Tab);
            const QQuickItem* at = window->activeFocusItem();
            if (at != nullptr && at->property("stream").isValid() && at->property("host").isValid()) {
                QTest::keyClick(window, Qt::Key_Return);
            }
        }
        QVERIFY(!wall.enlarged().isEmpty());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theGroupsMenuNarrowsTheWallToOneGroup()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);

        // The button says "All groups", and what drops from it names them, with a tick at what is chosen.
        const auto* button = window->findChild<QQuickItem*>(u"groupsButton"_s);
        QVERIFY(button != nullptr);
        QVERIFY(button->isVisible());
        QVERIFY(says(button, u"All groups"_s));
        QObject* menu = window->findChild<QObject*>(u"groupsMenu"_s);
        QVERIFY(menu != nullptr);
        QVERIFY(!menu->property("opened").toBool());
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        for (const QString& row : {u"All groups"_s, u"Office"_s, u"Rack A"_s, u"Rack B"_s}) {
            QVERIFY2(rowOf(menu, row) != nullptr, qPrintable(row));
        }
        keepPictureOf(window, u"menu-groups"_s);

        // One is chosen: the menu goes, the wall shows that group's machines, and the button says which.
        const QQuickItem* row = rowOf(menu, u"Rack A"_s);
        QVERIFY(row != nullptr);
        clickOn(window, row);
        QTRY_COMPARE(wall.shown()->group(), u"Rack A"_s);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 4);
        QTRY_VERIFY(says(button, u"Rack A"_s));

        // Esc closes it without choosing.
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(wall.shown()->group(), u"Rack A"_s);

        // And all of them again.
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        row = rowOf(menu, u"All groups"_s);
        QVERIFY(row != nullptr);
        clickOn(window, row);
        QTRY_COMPARE(wall.shown()->group(), QString());
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aBarsMenuThatIsLongerThanTheWindowScrollsInsideIt()
    {
        // Forty machines, each in a group of its own, in a window as low as a window gets (and as
        // wide as the bar needs to have a button for the groups at all).
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles;
        for (int number = 1; number <= 40; ++number) {
            qkvm::ui::WallTile tile;
            tile.key = u"m%1"_s.arg(number);
            tile.name = u"Machine %1"_s.arg(number);
            tile.group = u"Group %1"_s.arg(number, 2, 10, QLatin1Char('0'));
            tile.state = u"online"_s;
            tile.label = u"Online"_s;
            tiles.append(tile);
        }
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->setWidth(840);
        window->setHeight(window->minimumHeight());
        QVERIFY(bringUp(window));
        auto* button = window->findChild<QQuickItem*>(u"groupsButton"_s);
        QVERIFY(button != nullptr);
        QTRY_VERIFY(button->isVisible());
        QObject* menu = window->findChild<QObject*>(u"groupsMenu"_s);
        QVERIFY(menu != nullptr);
        const auto marked = [&](const QString& row) { return says(window->activeFocusItem(), row); };
        // What of the menu can be seen: its frame.
        const auto frame = [&] {
            const auto* rows = menu->property("contentItem").value<QQuickItem*>();
            return rows->mapRectToScene(QRectF(0, 0, rows->width(), rows->height()));
        };
        const auto seenInTheMenu = [&](const QQuickItem* row) {
            return row != nullptr && frame().contains(row->mapRectToScene(QRectF(0, 0, row->width(), row->height())));
        };

        button->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(menu->property("opened").toBool());
        // It ends inside the window, and begins at its first row, which is the chosen one.
        QTRY_VERIFY(frame().bottom() <= window->height());
        QVERIFY(frame().top() >= 0);
        QTRY_VERIFY(marked(u"All groups"_s));
        QVERIFY(seenInTheMenu(window->activeFocusItem()));
        QVERIFY(!seenInTheMenu(rowOf(menu, u"Group 40"_s)));
        keepPictureOf(window, u"menu-groups-long"_s);
        const auto* rows = menu->property("contentItem").value<QQuickItem*>();
        const QImage overTheBar = stripAbove(window, rows);
        QVERIFY(!overTheBar.isNull());
        // The keys bring a row into sight: up from the first is the last, and down from that a few more.
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(u"Group 40"_s));
        QTRY_VERIFY(seenInTheMenu(window->activeFocusItem()));
        QVERIFY(!seenInTheMenu(rowOf(menu, u"All groups"_s)));
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_VERIFY(marked(u"All groups"_s));
        QTRY_VERIFY(seenInTheMenu(window->activeFocusItem()));
        for (int down = 1; down <= 20; ++down) {
            QTest::keyClick(window, Qt::Key_Down);
        }
        QTRY_VERIFY(marked(u"Group 20"_s));
        QTRY_VERIFY(seenInTheMenu(window->activeFocusItem()));
        keepPictureOf(window, u"menu-groups-long-middle"_s);
        // The tick of "All groups", scrolled out over the menu's top, is not drawn over the bar.
        QVERIFY(stripAbove(window, rows) == overTheBar);
        // A row that is in sight is chosen with a click, where it is.
        const QQuickItem* above = rowOf(menu, u"Group 19"_s);
        QVERIFY(seenInTheMenu(above));
        clickOn(window, above);
        QTRY_COMPARE(wall.shown()->group(), u"Group 19"_s);
        QTRY_VERIFY(!menu->property("opened").toBool());

        // Opened again, it shows the one that is chosen.
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(marked(u"Group 19"_s));
        QTRY_VERIFY(seenInTheMenu(window->activeFocusItem()));
        // Scrolled until that row is just out over the menu's top: its tick is not drawn over the
        // bar, where the row would be.
        const QQuickItem* chosen = rowOf(menu, u"Group 19"_s)->parentItem()->parentItem();
        auto* scroller = menu->property("contentItem").value<QQuickItem*>();
        QVERIFY(scroller->setProperty(
            "contentY", chosen->mapToItem(scroller->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y() +
                            chosen->height() + 2));
        QVERIFY(stripAbove(window, rows) == overTheBar);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());

        // "View", the page's panel of choices, needs no scrolling in so low a window: it is whole
        // inside it, and its last choice is made where it is.
        menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(menu != nullptr);
        clickOn(window, theOne(window, u"viewButton"_s));
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(frame().bottom() <= window->height());
        QVERIFY(!menu->property("flick").value<QQuickItem*>()->property("scrolls").toBool());
        QVERIFY(seenInTheMenu(rowOf(menu, u"Order"_s)));
        QVERIFY(seenInTheMenu(rowOf(menu, u"New window"_s)));
        keepPictureOf(window, u"menu-view-low"_s);
        QVERIFY(chooseIn(window, u"paceChoice"_s, u"Paused"_s));
        QTRY_COMPARE(wall.pace(), 0);
        QVERIFY(!wall.displaying());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theViewMenuSetsTheOrderAndTheColumns()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const auto* button = window->findChild<QQuickItem*>(u"viewButton"_s);
        QVERIFY(button != nullptr);
        QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(menu != nullptr);
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);

        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        // As the page's View: each choice under its heading, saying what is chosen, and below them
        // the rows that lead somewhere.
        for (const QString& line : {u"Order"_s, u"Columns"_s, u"A new picture every"_s, u"Arrange the tiles…"_s,
                                    u"Display mode"_s, u"New window"_s}) {
            QVERIFY2(rowOf(menu, line) != nullptr, qPrintable(line));
        }
        QCOMPARE(chosenIn(window, u"orderChoice"_s), u"As arranged"_s);
        QCOMPARE(chosenIn(window, u"columnsChoice"_s), u"Fit"_s);
        QCOMPARE(chosenIn(window, u"paceChoice"_s), u"1 s"_s);
        // It is whole inside the window, though its button is near the window's right edge.
        const auto* inside = menu->property("flick").value<QQuickItem*>();
        QVERIFY(isInside(window, inside));
        QVERIFY(!inside->property("scrolls").toBool());
        keepPictureOf(window, u"menu-view"_s);
        // A choice is made at once, and the menu stays for the next one, as the page's does.
        QVERIFY(chooseIn(window, u"orderChoice"_s, u"By name"_s));
        QTRY_COMPARE(wall.shown()->order(), u"name"_s);
        QCOMPARE(chosenIn(window, u"orderChoice"_s), u"By name"_s);
        QVERIFY(menu->property("opened").toBool());

        // Fifteen machines in one column: the wall scrolls.
        QCOMPARE(scrolled->property("columns").toInt() > 1, true);
        QVERIFY(chooseIn(window, u"columnsChoice"_s, u"1"_s));
        QTRY_COMPARE(wall.columns(), 1);
        QTRY_COMPARE(scrolled->property("columns").toInt(), 1);
        QVERIFY(scrolled->property("contentHeight").toReal() > scrolled->height());
        QVERIFY(menu->property("opened").toBool());
        // Esc closes it.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        keepPictureOf(window, u"wall-one-column"_s);

        // How often a new picture is asked for: the page's paces, and the server's own among them.
        wall.setServerPace(3000);
        QCOMPARE(wall.pace(), 3000);
        QVERIFY(says(button, u"View"_s));
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        // What is chosen is the pace that holds: the server's, until another is chosen.
        QCOMPARE(chosenIn(window, u"paceChoice"_s), u"3 s"_s);
        QVERIFY(chooseIn(window, u"paceChoice"_s, u"5 s"_s));
        QTRY_COMPARE(wall.pace(), 5000);
        QCOMPARE(chosenIn(window, u"paceChoice"_s), u"5 s"_s);
        QVERIFY(says(button, u"View"_s));
        // Paused: the bar says so where it said "View", since a wall that stands still looks like
        // one that is up to date.
        QVERIFY(chooseIn(window, u"paceChoice"_s, u"Paused"_s));
        QTRY_COMPARE(wall.pace(), 0);
        QTRY_VERIFY(says(button, u"Paused"_s));
        QVERIFY(!says(button, u"View"_s));
        QCOMPARE(chosenIn(window, u"paceChoice"_s), u"Paused"_s);
        keepPictureOf(window, u"menu-view-paused"_s);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QVERIFY(says(button, u"Paused"_s));
        keepPictureOf(window, u"wall-paused"_s);
        // And a pace again: "View" again.
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(chooseIn(window, u"paceChoice"_s, u"0.5 s"_s));
        QTRY_COMPARE(wall.pace(), 500);
        QTRY_VERIFY(says(button, u"View"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aWindowThatShowsOneGroupSaysWhichInItsTitle()
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
        QCOMPARE(window->title(), u"qKVM"_s);
        // Of several windows, each with a group of its own, the taskbar says which is which.
        wall.shown()->setGroup(u"Rack B"_s);
        QTRY_COMPARE(window->title(), u"Rack B - qKVM"_s);
        // A group the wall does not have is not shown, and not named.
        wall.shown()->setGroup(u"Cellar"_s);
        QTRY_COMPARE(window->title(), u"qKVM"_s);
        wall.shown()->setGroup(u"Office"_s);
        QTRY_COMPARE(window->title(), u"Office - qKVM"_s);
        // Nobody is signed in: the program's name, and nothing of a wall that is not there.
        qkvm::ui::SessionView::Shown nobody;
        nobody.stage = u"signIn"_s;
        session.show(nobody);
        QTRY_COMPARE(window->title(), u"qKVM"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Where the wall may come from this PC or a server, the title says which, before the program's name, as
    // Windows titles go, and with nothing but hyphens between. (Qt puts " - " and the program's name after a title
    // that does not end with it: 0.1.0 said "qKVM " and a long dash, and Windows showed "qKVM <dash>  - qKVM"
    // before anything was chosen, and "qKVM <dash> This PC - qKVM" after.)
    void theTitleSaysWhichWallBeforeTheProgramsNameWithHyphensAlone()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        // Nothing chosen yet: neither this PC's wall nor an address.
        qkvm::ui::SessionView::Shown nothing;
        nothing.stage = u"signIn"_s;
        session.show(nothing);
        session.showSources({QVariantMap{{u"value"_s, u"pc"_s}, {u"label"_s, u"This PC"_s}},
                             QVariantMap{{u"value"_s, u"https://example.test/"_s}, {u"label"_s, u"example.test"_s}}},
                            u"pc"_s, true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QString name = QGuiApplication::applicationDisplayName();
        QCOMPARE(name, u"qKVM"_s);
        // Every title ends with the program's name, so that Qt adds nothing to it; and has no dash but the hyphen.
        const auto plain = [&](const QString& title) {
            return title.endsWith(name) && std::ranges::none_of(title, [](QChar sign) {
                       return sign != u'-' && (sign.category() == QChar::Punctuation_Dash ||
                                               sign.category() == QChar::Punctuation_InitialQuote ||
                                               sign.category() == QChar::Punctuation_FinalQuote);
                   });
        };
        QTRY_COMPARE(window->title(), u"qKVM"_s);
        // This PC's own wall.
        session.show(atItsOwnWall());
        QTRY_COMPARE(window->title(), u"This PC - qKVM"_s);
        QVERIFY(plain(window->title()));
        // A server's.
        session.show(signedIn());
        QTRY_COMPARE(window->title(), u"192.168.1.10:8787 - qKVM"_s);
        QVERIFY(plain(window->title()));
        // One group of it, in a window of its own.
        wall.shown()->setGroup(u"Rack B"_s);
        QTRY_COMPARE(window->title(), u"Rack B - 192.168.1.10:8787 - qKVM"_s);
        QVERIFY(plain(window->title()));
        session.show(atItsOwnWall());
        QTRY_COMPARE(window->title(), u"Rack B - This PC - qKVM"_s);
        QVERIFY(plain(window->title()));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void anotherWindowIsAskedForInTheViewMenuAndWithTheKeyboard()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        // The window says it to whoever keeps the program's windows: it opens none itself.
        const QSignalSpy asked(window, SIGNAL(windowAsked()));
        QVERIFY(asked.isValid());
        const auto* button = window->findChild<QQuickItem*>(u"viewButton"_s);
        QVERIFY(button != nullptr);
        QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(menu != nullptr);

        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(rowOf(menu, u"New window"_s) != nullptr);
        keepPictureOf(window, u"menu-view-new-window"_s);
        clickOn(window, rowOf(menu, u"New window"_s));
        QTRY_COMPARE(asked.count(), 1);
        QTRY_VERIFY(!menu->property("opened").toBool());
        // And nothing of this window's wall is other than it was.
        QVERIFY(!wall.displaying());
        QCOMPARE(wall.columns(), 0);

        // With the keys, as "New window" is in every program.
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(asked.count(), 2);
        // Also where the wall is only shown, and has no bar to ask in.
        wall.setDisplaying(true);
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(asked.count(), 3);
        // And a plain N is a letter, and asks for nothing.
        QTest::keyClick(window, Qt::Key_N);
        QCOMPARE(asked.count(), 3);

        // Before anybody is signed in there is no wall to show in another window.
        qkvm::ui::SessionView::Shown nobody;
        nobody.stage = u"signIn"_s;
        session.show(nobody);
        QTRY_VERIFY(!window->findChild<QQuickItem*>(u"wall"_s)->isVisible());
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(asked.count(), 3);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void anAdminArrangesTheTilesByHand()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(fiveMachines());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QQuickItem* all = window->contentItem();
        QTRY_VERIFY(cardOf(all, u"e"_s) != nullptr);
        const QQuickItem* foot = theOne(window, u"arrangeBar"_s);
        QVERIFY(foot != nullptr);
        QVERIFY(!foot->isVisible());
        QCOMPARE(seenNamed(window, u"cardArrange"_s), 0);
        const auto* button = window->findChild<QQuickItem*>(u"viewButton"_s);
        QVERIFY(button != nullptr);
        QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(menu != nullptr);
        const auto placeSaid = [&](const QString& key) {
            const QQuickItem* field = inCard(cardOf(all, key), u"placeField"_s);
            return field == nullptr ? QString() : field->property("text").toString();
        };

        // In the View menu, for an admin.
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(rowOf(menu, u"Arrange the tiles…"_s) != nullptr);
        clickOn(window, rowOf(menu, u"Arrange the tiles…"_s));
        QTRY_VERIFY(wall.arranging());
        QTRY_VERIFY(!menu->property("opened").toBool());
        // Every card says its place, and the bar at the foot of the wall says what is being done.
        QTRY_COMPARE(seenNamed(window, u"cardArrange"_s), 5);
        QVERIFY(foot->isVisible());
        QVERIFY(says(foot, u"Arrange"_s));
        QVERIFY(says(foot, u"Drag a tile to its place, or type the place it is to have."_s));
        QVERIFY(says(foot, u"Cancel"_s));
        QVERIFY(says(foot, u"Save this order"_s));
        QVERIFY(foot->mapToScene(QPointF(0, foot->height())).y() <= window->height());
        QCOMPARE(placeSaid(u"a"_s), u"1"_s);
        QCOMPARE(placeSaid(u"e"_s), u"5"_s);
        // The cards make room for that bar, as they do for the one of picked machines.
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        QCOMPARE(scrolled->property("foot").toReal(), 64.0);
        keepPictureOf(window, u"wall-arranging"_s);
        // Nothing else is done meanwhile: the bar rests, and so do the wall's own keys; a card is
        // not opened, and has no way into a menu.
        clickOn(window, button);
        QTest::qWait(150);
        QVERIFY(!menu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_S);
        QVERIFY(!wall.displaying());
        QVERIFY(!wall.selecting());
        QTest::mouseMove(window, cardOf(all, u"c"_s)->mapToScene(QPointF(30, 30)).toPoint());
        QTest::qWait(100);
        QCOMPARE(seenNamed(window, u"cardActions"_s), 0);
        QTest::mouseDClick(window, Qt::LeftButton, {}, cardOf(all, u"c"_s)->mapToScene(QPointF(30, 30)).toPoint());
        QTest::qWait(100);
        QCOMPARE(wall.enlarged(), QString());

        // A step to either side, by the card's two buttons; the first has nowhere earlier to go,
        // and the last nowhere later.
        clickOn(window, inCard(cardOf(all, u"a"_s), u"placeLater"_s));
        QTRY_COMPARE(made(wall), (QStringList{u"b"_s, u"a"_s, u"c"_s, u"d"_s, u"e"_s}));
        QTRY_COMPARE(placeSaid(u"a"_s), u"2"_s);
        QCOMPARE(placeSaid(u"b"_s), u"1"_s);
        clickOn(window, inCard(cardOf(all, u"a"_s), u"placeEarlier"_s));
        QTRY_COMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        QVERIFY(!inCard(cardOf(all, u"a"_s), u"placeEarlier"_s)->isEnabled());
        QVERIFY(!inCard(cardOf(all, u"e"_s), u"placeLater"_s)->isEnabled());
        QVERIFY(inCard(cardOf(all, u"c"_s), u"placeLater"_s)->isEnabled());

        // The place typed in: what stood there is taken away by the first key.
        const QQuickItem* field = inCard(cardOf(all, u"e"_s), u"placeField"_s);
        QVERIFY(field != nullptr);
        clickOn(window, field);
        QTRY_VERIFY(field->hasActiveFocus());
        const QQuickItem* aCard = cardOf(all, u"c"_s);
        QTest::keyClick(window, Qt::Key_2);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(made(wall), (QStringList{u"a"_s, u"e"_s, u"b"_s, u"c"_s, u"d"_s}));
        QTRY_COMPARE(placeSaid(u"e"_s), u"2"_s);
        QCOMPARE(placeSaid(u"d"_s), u"5"_s);
        // Every card is still the card it was: the one that was typed into has the keyboard for
        // the next number, and the others were seen to make room.
        QCOMPARE(cardOf(all, u"c"_s), aCard);
        QCOMPARE(inCard(cardOf(all, u"e"_s), u"placeField"_s), field);
        QVERIFY(field->hasActiveFocus());
        // A number that is no place is the nearest one; and nothing typed is nothing asked: the
        // card says the place it has.
        field = inCard(cardOf(all, u"a"_s), u"placeField"_s);
        clickOn(window, field);
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_9);
        QTest::keyClick(window, Qt::Key_9);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(made(wall), (QStringList{u"e"_s, u"b"_s, u"c"_s, u"d"_s, u"a"_s}));
        QTRY_COMPARE(placeSaid(u"a"_s), u"5"_s);
        QVERIFY(field->hasActiveFocus());
        // Typed into the card that is last already, it moves nothing, and the card says its
        // place and not what was typed.
        QTest::keyClick(window, Qt::Key_9);
        QTRY_COMPARE(placeSaid(u"a"_s), u"59"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(placeSaid(u"a"_s), u"5"_s);
        QCOMPARE(made(wall), (QStringList{u"e"_s, u"b"_s, u"c"_s, u"d"_s, u"a"_s}));
        field = inCard(cardOf(all, u"c"_s), u"placeField"_s);
        clickOn(window, field);
        QTRY_VERIFY(field->hasActiveFocus());
        // (The wall's own keys rest: "/" is no way to "Find a KVM" from here.)
        QTest::keyClick(window, Qt::Key_Slash);
        QTest::qWait(50);
        QVERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Backspace);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(100);
        QCOMPARE(made(wall), (QStringList{u"e"_s, u"b"_s, u"c"_s, u"d"_s, u"a"_s}));
        QCOMPARE(placeSaid(u"c"_s), QString());
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(!field->hasActiveFocus());
        QTRY_COMPARE(placeSaid(u"c"_s), u"3"_s);

        // Dragged: taken hold of beside its buttons, over the earlier half of another card, and let
        // go there. While it is dragged it is faint, its name goes with the pointer, and a line
        // says where it would come.
        const QQuickItem* dragged = cardOf(all, u"a"_s);
        const QQuickItem* over = cardOf(all, u"b"_s);
        const QPoint from = dragged->mapToScene(QPointF(24, 24)).toPoint();
        QPoint to = over->mapToScene(QPointF(over->width() * 0.25, 30)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, from);
        QTest::mouseMove(window, from + QPoint(40, 10));
        QTest::mouseMove(window, to + QPoint(5, 5));
        QTest::mouseMove(window, to);
        QTRY_COMPARE(seenNamed(window, u"dropLine"_s), 1);
        QVERIFY(dragged->opacity() < 1);
        QTRY_VERIFY(theOne(window, u"draggedCard"_s)->isVisible());
        QVERIFY(says(theOne(window, u"draggedCard"_s), u"Machine A"_s));
        QCOMPARE(inCard(over, u"dropLine"_s)->x(), 0.0);
        keepPictureOf(window, u"wall-arranging-dragged"_s);
        QTest::mouseRelease(window, Qt::LeftButton, {}, to);
        QTRY_COMPARE(made(wall), (QStringList{u"e"_s, u"a"_s, u"b"_s, u"c"_s, u"d"_s}));
        QTRY_COMPARE(seenNamed(window, u"dropLine"_s), 0);
        QVERIFY(!theOne(window, u"draggedCard"_s)->isVisible());
        QTRY_COMPARE(cardOf(all, u"a"_s)->opacity(), 1.0);
        // Over the later half of a card it comes after that one.
        QTRY_COMPARE(seenNamed(window, u"cardArrange"_s), 5);
        QTest::qWait(300);
        dragged = cardOf(all, u"e"_s);
        over = cardOf(all, u"b"_s);
        const QPoint fromFirst = dragged->mapToScene(QPointF(24, 24)).toPoint();
        to = over->mapToScene(QPointF(over->width() * 0.75, 30)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, fromFirst);
        QTest::mouseMove(window, fromFirst + QPoint(40, 10));
        QTest::mouseMove(window, to + QPoint(5, 5));
        QTest::mouseMove(window, to);
        QTRY_COMPARE(seenNamed(window, u"dropLine"_s), 1);
        QVERIFY(inCard(over, u"dropLine"_s)->x() > 0);
        QTest::mouseRelease(window, Qt::LeftButton, {}, to);
        QTRY_COMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"e"_s, u"c"_s, u"d"_s}));
        // A press that goes nowhere is no drag, and a card let go over itself stays.
        QTest::qWait(300);
        const QPoint still = cardOf(all, u"c"_s)->mapToScene(QPointF(24, 24)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, still);
        QTest::mouseRelease(window, Qt::LeftButton, {}, still);
        // Nor is one that moves by a hair, as a hand does when it clicks.
        QTest::mousePress(window, Qt::LeftButton, {}, still);
        QTest::mouseMove(window, still + QPoint(1, 1));
        QTest::mouseMove(window, still + QPoint(2, 1));
        QTest::qWait(50);
        QVERIFY(!theOne(window, u"draggedCard"_s)->isVisible());
        QCOMPARE(cardOf(all, u"c"_s)->opacity(), 1.0);
        QTest::mouseRelease(window, Qt::LeftButton, {}, still + QPoint(2, 1));
        // Dragged about over itself, into its later half, and let go there: no line, and no move.
        const QPoint itsLaterHalf =
            cardOf(all, u"c"_s)->mapToScene(QPointF(cardOf(all, u"c"_s)->width() * 0.8, 30)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, still);
        QTest::mouseMove(window, still + QPoint(40, 10));
        QTest::mouseMove(window, itsLaterHalf + QPoint(-3, 0));
        QTest::mouseMove(window, itsLaterHalf);
        QTRY_VERIFY(theOne(window, u"draggedCard"_s)->isVisible());
        QCOMPARE(seenNamed(window, u"dropLine"_s), 0);
        QTest::mouseRelease(window, Qt::LeftButton, {}, itsLaterHalf);
        QTest::qWait(100);
        QCOMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"e"_s, u"c"_s, u"d"_s}));
        QCOMPARE(seenNamed(window, u"dropLine"_s), 0);

        // "Save this order": asked once, with the order as it was made; until the server has
        // answered the button waits and no card moves.
        const QSignalSpy asked(&wall, &qkvm::ui::WallView::orderAsked);
        const QQuickItem* save = theOne(window, u"arrangeSave"_s);
        QVERIFY(save != nullptr);
        clickOn(window, save);
        QTRY_COMPARE(asked.count(), 1);
        QCOMPARE(asked.at(0).at(0).toStringList(), (QStringList{u"a"_s, u"b"_s, u"e"_s, u"c"_s, u"d"_s}));
        QTRY_VERIFY(!save->isEnabled());
        QVERIFY(!inCard(cardOf(all, u"b"_s), u"placeLater"_s)->isEnabled());
        clickOn(window, save);
        QTest::qWait(100);
        QCOMPARE(asked.count(), 1);
        const QPoint held = cardOf(all, u"a"_s)->mapToScene(QPointF(24, 24)).toPoint();
        const QPoint another =
            cardOf(all, u"e"_s)->mapToScene(QPointF(cardOf(all, u"e"_s)->width() * 0.8, 30)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, {}, held);
        QTest::mouseMove(window, held + QPoint(40, 10));
        QTest::mouseMove(window, another + QPoint(-3, 0));
        QTest::mouseMove(window, another);
        QTest::qWait(100);
        QVERIFY(!theOne(window, u"draggedCard"_s)->isVisible());
        QCOMPARE(seenNamed(window, u"dropLine"_s), 0);
        QTest::mouseRelease(window, Qt::LeftButton, {}, another);
        QCOMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"e"_s, u"c"_s, u"d"_s}));
        // The server took it: the wall is the wall again, in the order that was made, and its bar
        // works.
        wall.machines()->setTiles({fiveMachines().at(0), fiveMachines().at(1), fiveMachines().at(4),
                                   fiveMachines().at(2), fiveMachines().at(3)});
        wall.orderSaved();
        QTRY_VERIFY(!foot->isVisible());
        QTRY_COMPARE(seenNamed(window, u"cardArrange"_s), 0);
        QCOMPARE(scrolled->property("foot").toReal(), 0.0);
        QCOMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"e"_s, u"c"_s, u"d"_s}));
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void anOrderIsGivenUpWithCancelOrEscAndOnlyAnAdminIsOfferedIt()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(fiveMachines());
        wall.shown()->setOrder(u"name"_s);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QQuickItem* all = window->contentItem();
        QTRY_VERIFY(cardOf(all, u"e"_s) != nullptr);
        const QQuickItem* foot = theOne(window, u"arrangeBar"_s);
        QVERIFY(foot != nullptr);

        wall.arrange();
        QTRY_COMPARE(seenNamed(window, u"cardArrange"_s), 5);
        // Every card is at its place from the first moment: none is seen to fly there.
        QVERIFY(cardOf(all, u"b"_s)->x() > cardOf(all, u"a"_s)->x());
        QVERIFY(cardOf(all, u"e"_s)->y() > cardOf(all, u"a"_s)->y());
        clickOn(window, inCard(cardOf(all, u"a"_s), u"placeLater"_s));
        QTRY_COMPARE(made(wall), (QStringList{u"b"_s, u"a"_s, u"c"_s, u"d"_s, u"e"_s}));
        // "Cancel": the wall as it was, by name as the bar had it.
        clickOn(window, theOne(window, u"arrangeCancel"_s));
        QTRY_VERIFY(!wall.arranging());
        QTRY_VERIFY(!foot->isVisible());
        QCOMPARE(wall.shown()->order(), u"name"_s);
        QCOMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        // And Esc is the same way out, also from a card's number.
        wall.arrange();
        QTRY_COMPARE(seenNamed(window, u"cardArrange"_s), 5);
        const QQuickItem* field = inCard(cardOf(all, u"d"_s), u"placeField"_s);
        clickOn(window, field);
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_1);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!wall.arranging());
        QCOMPARE(made(wall), (QStringList{u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
        QTRY_COMPARE(seenNamed(window, u"cardArrange"_s), 0);

        // Whoever is no admin finds no such row: the wall's own order is everybody's.
        qkvm::ui::SessionView::Shown operating = signedIn();
        operating.manages = false;
        session.show(operating);
        const auto* button = window->findChild<QQuickItem*>(u"viewButton"_s);
        QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(button != nullptr && menu != nullptr);
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(rowOf(menu, u"Arrange the tiles…"_s) == nullptr);
        QVERIFY(rowOf(menu, u"Display mode"_s) != nullptr);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aMenuIsWorkedWithTheKeyboard()
    {
        // A menu of rows (here More, in the narrowest window, where it has a heading and two lines
        // between its rows), and the View, whose rows are the page's choices.
        // (The pointer over a button is known, as on Windows: a button tells a click from the keys
        // by it, and Qt's off-screen platform has no hover of its own.)
        QStyleHints* hints = QGuiApplication::styleHints();
        const bool hovers = hints->useHoverEffects();
        hints->setUseHoverEffects(true);
        const auto asItWas = qScopeGuard([&] { hints->setUseHoverEffects(hovers); });
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->resize(640, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), 640);
        auto* button = window->findChild<QQuickItem*>(u"moreButton"_s);
        QVERIFY(button != nullptr && isSeen(button));
        QObject* menu = window->findChild<QObject*>(u"moreMenu"_s);
        QVERIFY(menu != nullptr);
        const auto marked = [&](const QString& row) { return says(window->activeFocusItem(), row); };
        const QStringList groups = wall.machines()->groups();
        QVERIFY(groups.size() >= 2);
        const QString theme = m_looks.back()->themeHint();
        // Where the keys are can be seen, as the page's outline shows it (Qt's visualFocus).
        const auto shown = [&](const QQuickItem* item) {
            return item != nullptr && item->hasActiveFocus() && item->property("visualFocus").toBool();
        };

        // Opened from its button, the keys start at what is chosen.
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(button->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(marked(u"All groups"_s));
        QVERIFY(shown(window->activeFocusItem()));
        // Esc closes it, and the keys are where they were, and are seen there.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTRY_VERIFY(shown(button));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(marked(u"All groups"_s));
        // Down the rows, and over a line to the next kind of row.
        for (const QString& row : groups + QStringList{u"Fullscreen"_s, theme, u"Settings"_s}) {
            QTest::keyClick(window, Qt::Key_Down);
            QTRY_VERIFY2(marked(row), qPrintable(row));
        }
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(theme));
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(u"Fullscreen"_s));
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(groups.constLast()));
        // Return chooses: the menu goes, and the keys are at its button again.
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(wall.shown()->group(), groups.constLast());
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTRY_VERIFY(shown(button));

        // Round the ends, and over the heading: up from the first row is the last one, and down
        // from that the first.
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(marked(groups.constLast()));
        for (int up = 1; up < groups.size(); ++up) {
            QTest::keyClick(window, Qt::Key_Up);
        }
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(u"All groups"_s));
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(u"Settings"_s));
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_VERIFY(marked(u"All groups"_s));
        // And Space chooses as Return does.
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(wall.shown()->group(), QString());
        QTRY_VERIFY(!menu->property("opened").toBool());

        // The View, from its button: the keys start at its first choice, and are seen there. Up
        // and Down go from part to part, round the ends, and so does Tab. Return opens a choice's
        // list where the chosen one is marked; Return chooses there, the View stays for the next
        // choice, and the keys are seen at the choice again. Esc closes a list and leaves the View
        // open, and Esc again closes the View: the keys are at its button again, and seen there.
        window->resize(1280, 800);
        QTRY_COMPARE(window->width(), 1280);
        button = window->findChild<QQuickItem*>(u"viewButton"_s);
        menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(button != nullptr && menu != nullptr);
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(button->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"orderChoice"_s);
        QVERIFY(shown(window->activeFocusItem()));
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(marked(u"New window"_s));
        QVERIFY(shown(window->activeFocusItem()));
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"orderChoice"_s);
        QTest::keyClick(window, Qt::Key_Down);
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"columnsChoice"_s);
        QVERIFY(shown(window->activeFocusItem()));
        // (Down moved, and opened nothing.)
        QCOMPARE(seenNamed(window, u"choice"_s), 0);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 0);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(wall.columns(), 2);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QVERIFY(menu->property("opened").toBool());
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"columnsChoice"_s);
        QTRY_VERIFY(shown(window->activeFocusItem()));
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"paceChoice"_s);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 0);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QVERIFY(menu->property("opened").toBool());
        QTRY_VERIFY(shown(window->activeFocusItem()));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTRY_VERIFY(shown(button));
        // Its button has the keys from the keyboard, and the pointer comes to it and clicks: that
        // is a click, and nothing in the panel is outlined.
        QTest::mouseMove(window, button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"orderChoice"_s);
        QVERIFY(!window->activeFocusItem()->property("visualFocus").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        // Opened with the pointer, the keys are not shown: nobody is at the keyboard. (The keys
        // are elsewhere first, as after a click on the wall: a button that has them from the
        // keyboard keeps showing them through a click, as Qt's do.)
        window->findChild<QQuickItem*>(u"wall"_s)->forceActiveFocus(Qt::MouseFocusReason);
        QTest::mouseMove(window, button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"orderChoice"_s);
        QVERIFY(!window->activeFocusItem()->property("visualFocus").toBool());
        // And a click on its button again closes it, as the page's toggles.
        clickOn(window, button);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTest::qWait(150);
        QVERIFY(!menu->property("opened").toBool());
        QCOMPARE(wall.shown()->order(), QString());
        QCOMPARE(wall.pace(), 1000);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatWasDoneIsToldInTheCornerOfTheWindow()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const auto* list = window->findChild<QQuickItem*>(u"notices"_s);
        QVERIFY(list != nullptr);
        QCOMPARE(seenNamed(window, u"notice"_s), 0);

        actions.notices()->tell(qkvm::ui::Notices::Kind::Done, u"NAS: power button pressed"_s);
        actions.notices()->tell(
            qkvm::ui::Notices::Kind::Failed,
            u"NAS: no answer to \"Press power button\". It may have been done all the same: look before sending it again."_s);
        const int underWay = actions.notices()->underWay(u"Rack server: Restart controller…"_s);
        QTRY_COMPARE(seenNamed(window, u"notice"_s), 3);
        QVERIFY(saying(window->contentItem(), u"NAS: power button pressed"_s) != nullptr);
        QVERIFY(saying(window->contentItem(), u"Rack server: Restart controller…"_s) != nullptr);
        // In the lower right corner, whole, and no wider than a notice is.
        QTRY_COMPARE(list->mapToScene(QPointF(list->width(), list->height())).toPoint(),
                     QPoint(window->width() - 16, window->height() - 16));
        QVERIFY(list->width() <= 400);
        QVERIFY(list->mapToScene(QPointF(0, 0)).y() > 0);
        // A long sentence is on several lines, inside its notice.
        const QQuickItem* longOne = saying(
            window->contentItem(),
            u"NAS: no answer to \"Press power button\". It may have been done all the same: look before sending it again."_s);
        QVERIFY(longOne != nullptr);
        QVERIFY(longOne->height() > 30);
        QVERIFY(longOne->height() <= longOne->parentItem()->height());
        keepPictureOf(window, u"notices"_s);

        // Over an open dialog too: what was done from a menu is told while the menu is there. They
        // are drawn after it, and so on top of it.
        actions.setAllowed(true);
        actions.showMenu(menuOfAKvm());
        const QObject* menu = window->findChild<QObject*>(u"machineMenu"_s);
        QTRY_VERIFY(menu->property("opened").toBool());
        QCOMPARE(seenNamed(window, u"notice"_s), 3);
        QTRY_COMPARE(list->mapToScene(QPointF(list->width(), list->height())).toPoint(),
                     QPoint(window->width() - 16, window->height() - 16));
        const QQuickItem* row = theOne(window, u"row:enlarge"_s);
        QVERIFY(row != nullptr);
        const auto layerOf = [&](const QQuickItem* item) {
            // What it is drawn with, among the things that lie over the window.
            const QQuickItem* layer = item;
            while (layer->parentItem() != nullptr && layer->parentItem() != list->parentItem()) {
                layer = layer->parentItem();
            }
            return layer;
        };
        QCOMPARE(layerOf(row)->parentItem(), list->parentItem());
        QVERIFY(list->z() > layerOf(row)->z());
        keepPictureOf(window, u"notices-over-menu"_s);
        actions.showMenu({});
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(seenNamed(window, u"notice"_s), 3);

        // What is over is gone.
        actions.notices()->over(underWay);
        QTRY_COMPARE(seenNamed(window, u"notice"_s), 2);
        actions.notices()->clear();
        QTRY_COMPARE(seenNamed(window, u"notice"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whoMayActFindsTheWayIntoAMachinesMenuOnItsCard()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QSignalSpy openAsked(&actions, &qkvm::ui::ActionsView::openAsked);
        const QSignalSpy portAsked(&actions, &qkvm::ui::ActionsView::portAsked);

        // The pointer is on the first card. Whoever may only look finds no way into a menu there.
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        const QPointF onTheFirstCard =
            scrolled->mapToScene(QPointF((scrolled->width() - scrolled->property("gridWidth").toReal()) / 2 +
                                             scrolled->property("cardWidth").toReal() / 2,
                                         scrolled->property("gridTop").toReal() + 60));
        QTest::mouseMove(window, onTheFirstCard.toPoint());
        QTest::qWait(150);
        QCOMPARE(seenNamed(window, u"cardActions"_s), 0);
        // Whoever may act finds it, on that card and on no other.
        actions.setAllowed(true);
        QTRY_COMPARE(seenNamed(window, u"cardActions"_s), 1);
        keepPictureOf(window, u"card-tool"_s);
        QList<const QQuickItem*> tools;
        named(window->contentItem(), u"cardActions"_s, tools);
        const auto shownTool = std::ranges::find_if(tools, [](const QQuickItem* tool) { return tool->isVisible(); });
        QVERIFY(shownTool != tools.end());
        clickOn(window, *shownTool);
        QTRY_COMPARE(openAsked.count(), 1);
        QCOMPARE(openAsked[0][0].toString(), tiles[0].key);
        // (A click on it is not a click on the card: the machine is not shown large by it.)
        QCOMPARE(wall.enlarged(), QString());

        // The numbers on the card of a KVM with several computers behind it ask for that computer.
        const auto several =
            std::ranges::find_if(tiles, [](const qkvm::ui::WallTile& tile) { return !tile.ports.isEmpty(); });
        QVERIFY(several != tiles.end());
        QList<const QQuickItem*> ports;
        named(window->contentItem(), u"port"_s, ports);
        QCOMPARE(ports.size(), several->ports.size());
        clickOn(window, ports[2]);
        QTRY_COMPARE(portAsked.count(), 1);
        QCOMPARE(portAsked[0][0].toString(), several->key);
        QCOMPARE(portAsked[0][1].toInt(), 2);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void everybodyFindsTheWayIntoAMachinesOwnConsole()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        // Whoever is signed in may only look.
        qkvm::ui::ActionsView actions;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QSignalSpy consoleAsked(&actions, &qkvm::ui::ActionsView::consoleAsked);
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        const auto onCard = [&](int column, int row) {
            const qreal width = scrolled->property("cardWidth").toReal();
            const qreal gap = scrolled->property("gap").toReal();
            return scrolled
                ->mapToScene(QPointF((scrolled->width() - scrolled->property("gridWidth").toReal()) / 2 +
                                         column * (width + gap) + width / 2,
                                     scrolled->property("gridTop").toReal() +
                                         row * (scrolled->property("cardHeight").toReal() + gap) + 60))
                .toPoint();
        };

        // The pointer is on the first card: the way into its console is there, and no menu's.
        QTest::mouseMove(window, onCard(0, 0));
        QTRY_COMPARE(seenNamed(window, u"cardConsole"_s), 1);
        QCOMPARE(seenNamed(window, u"cardActions"_s), 0);
        QList<const QQuickItem*> doors;
        named(window->contentItem(), u"cardConsole"_s, doors);
        const auto shownDoor = std::ranges::find_if(doors, [](const QQuickItem* door) { return door->isVisible(); });
        QVERIFY(shownDoor != doors.end());
        clickOn(window, *shownDoor);
        QTRY_COMPARE(consoleAsked.count(), 1);
        QCOMPARE(consoleAsked[0][0].toString(), tiles[0].key);
        QCOMPARE(wall.enlarged(), QString());

        // The fifth machine has no console a browser could be sent to: its card has no way there.
        // (The first card keeps its own in sight: the keyboard is on it since the click.)
        QVERIFY(!tiles[4].openable);
        QTest::mouseMove(window, onCard(0, 1));
        QTest::qWait(150);
        const auto doorsBelow = [&](int y) {
            QList<const QQuickItem*> all;
            named(window->contentItem(), u"cardConsole"_s, all);
            return std::ranges::count_if(all, [&](const QQuickItem* door) {
                return door->isVisible() && door->mapToScene(QPointF(0, 0)).y() > y;
            });
        };
        QCOMPARE(doorsBelow(onCard(0, 1).y() - 60), 0);
        // The sixth has one, and shows it under the pointer.
        QTest::mouseMove(window, onCard(1, 1));
        QTRY_COMPARE(doorsBelow(onCard(0, 1).y() - 60), 1);

        // Whoever may act finds both on the card under the pointer, side by side, neither over the
        // other.
        actions.setAllowed(true);
        QTest::mouseMove(window, onCard(2, 1));
        QList<QRectF> places;
        QTRY_VERIFY([&] {
            QList<const QQuickItem*> tools;
            named(window->contentItem(), u"cardActions"_s, tools);
            named(window->contentItem(), u"cardConsole"_s, tools);
            places.clear();
            for (const QQuickItem* tool : std::as_const(tools)) {
                const QRectF place = tool->mapRectToScene(QRectF(0, 0, tool->width(), tool->height()));
                if (tool->isVisible() && place.top() > onCard(0, 1).y() - 60) {
                    places.append(place);
                }
            }
            return places.size() == 2;
        }());
        QVERIFY(!places[0].intersects(places[1]));
        QCOMPARE(places[0].top(), places[1].top());
        keepPictureOf(window, u"card-tools"_s);

        // And in the bar of a machine shown large; not for the one without a console.
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedConsole"_s) != nullptr);
        QVERIFY(theOne(window, u"enlargedConsole"_s)->isVisible());
        clickOn(window, theOne(window, u"enlargedConsole"_s));
        QTRY_COMPARE(consoleAsked.count(), 2);
        QCOMPARE(consoleAsked[1][0].toString(), tiles[1].key);
        wall.closeEnlarged();
        wall.enlarge(tiles[4].key);
        QTRY_VERIFY(theOne(window, u"enlargedConsole"_s) != nullptr);
        QTRY_VERIFY(!theOne(window, u"enlargedConsole"_s)->isVisible());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theScreensAskNothingOfTheNetwork()
    {
        using qkvm::core::testing::Answered;
        using qkvm::core::testing::Asked;
        qkvm::core::testing::StandIn elsewhere([](const Asked&) { return Answered{200, "no picture", "image/png"}; });
        const QByteArray screen =
            "import QtQuick\nImage { source: \"" + elsewhere.address().toUtf8() + "/picture.png\" }\n";

        // As Qt has it, a picture that is named by an address is fetched.
        {
            QQmlEngine asIs;
            QQmlComponent component(&asIs);
            component.setData(screen, QUrl());
            const std::unique_ptr<QObject> made(component.create());
            QVERIFY2(made != nullptr, qPrintable(component.errorString()));
            // (Qt left to itself asks Windows first whether there is a proxy to go through, and
            // Windows can take several seconds over that the first time it is asked.)
            QTRY_COMPARE_WITH_TIMEOUT(elsewhere.asked.size(), 1, 30'000);
        }
        // As the program has it, nothing is: the picture says that it could not be had.
        QQmlEngine kept;
        qkvm::ui::keepOffTheNetwork(kept);
        QQmlComponent component(&kept);
        component.setData(screen, QUrl());
        const std::unique_ptr<QObject> made(component.create());
        QVERIFY2(made != nullptr, qPrintable(component.errorString()));
        // (3 is Image.Error.)
        QTRY_COMPARE(made->property("status").toInt(), 3);
        QTest::qWait(200);
        QCOMPARE(elsewhere.asked.size(), 1);
    }

    void whatComesFromOutsideIsShownAsLettersAndNeverAsAPage()
    {
        // A machine's name, an address, what a device says of itself, a refusal, a text somebody
        // typed: all of it comes from outside the program, and some of it from a device that
        // anybody may have named. Written like a page, it is still only letters: nothing of it is
        // drawn as a picture, and nothing is fetched for it.
        using qkvm::core::testing::Answered;
        using qkvm::core::testing::Asked;
        qkvm::core::testing::StandIn elsewhere([](const Asked&) { return Answered{200, "no picture", "image/png"}; });
        const QString page =
            u"<b>bold</b> <img src=\"%1/seen.png\" width=\"40\" height=\"40\">"_s.arg(elsewhere.address());

        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        for (qkvm::ui::WallTile& tile : tiles) {
            tile.name = page;
            tile.host = page;
            tile.group = page;
            tile.label = page;
            tile.meta = page;
            if (!tile.title.isEmpty()) {
                tile.title = page;
                tile.text = page;
                tile.action = tile.action.isEmpty() ? QString() : page;
            }
            if (!tile.server.isEmpty()) {
                tile.server[u"hardware"_s] = page;
                tile.server[u"tag"_s] = page;
                tile.server[u"readings"_s] =
                    QVariantList{QVariantMap{{u"label"_s, page}, {u"value"_s, page}, {u"tone"_s, QString()}}};
            }
        }
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown = signedIn();
        shown.account = page;
        shown.address = page;
        session.show(shown);
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);

        // The boxes that show what a server said, all open at once; the notices; one machine large.
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject*>(u"accountMenu"_s), "open"));
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject*>(u"aboutBox"_s), "open"));
        const QVariantMap row{{u"id"_s, u"one"_s}, {u"icon"_s, u"sun"_s}, {u"label"_s, page},
                              {u"hint"_s, page},   {u"danger"_s, false},  {u"enabled"_s, true}};
        actions.showMenu(
            {{u"title"_s, page},
             {u"about"_s, page},
             {u"warning"_s, page},
             {u"kindNote"_s, page},
             {u"sections"_s,
              QVariantList{QVariantMap{{u"title"_s, page}, {u"note"_s, page}, {u"rows"_s, QVariantList{row}}}}}});
        actions.showQuestion({{u"title"_s, page}, {u"text"_s, page}, {u"warning"_s, page}, {u"danger"_s, true}});
        actions.notices()->tell(qkvm::ui::Notices::Kind::Failed, page);
        actions.notices()->underWay(page);
        QTRY_VERIFY(window->findChild<QObject*>(u"question"_s)->property("opened").toBool());
        QTRY_COMPARE(seenNamed(window, u"notice"_s), 2);
        QTest::qWait(500);
        QVERIFY2(linesThatCouldBeAPage(window).isEmpty(), qPrintable(linesThatCouldBeAPage(window).join(u" | "_s)));
        actions.showQuestion({});
        actions.showMenu({});
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedName"_s) != nullptr);
        wall.closeEnlarged();
        wall.enlarge(tiles[12].key);
        QTest::qWait(300);
        QVERIFY2(linesThatCouldBeAPage(window).isEmpty(), qPrintable(linesThatCouldBeAPage(window).join(u" | "_s)));
        QList<const QQuickItem*> all;
        lines(window->contentItem(), all);
        QVERIFY2(all.size() > 150, qPrintable(QString::number(all.size())));

        // And where nobody is signed in yet: the address that was typed, and why it was refused.
        qkvm::ui::WallView emptyWall;
        qkvm::ui::SessionView asking;
        qkvm::ui::SessionView::Shown refused;
        refused.stage = u"signIn"_s;
        refused.address = page;
        refused.problem = page;
        refused.asksName = true;
        refused.asksCode = true;
        asking.show(refused);
        QQmlApplicationEngine second;
        QQuickWindow* connecting = load(second, about, asking, emptyWall);
        QVERIFY(connecting != nullptr);
        QVERIFY(bringUp(connecting));
        refused.stage = u"trust"_s;
        refused.certificate = page;
        refused.certificateBefore = page;
        asking.show(refused);
        QTest::qWait(300);
        QVERIFY2(linesThatCouldBeAPage(connecting).isEmpty(),
                 qPrintable(linesThatCouldBeAPage(connecting).join(u" | "_s)));
        refused.stage = u"address"_s;
        refused.suggested = page;
        asking.show(refused);
        QTest::qWait(300);
        QVERIFY2(linesThatCouldBeAPage(connecting).isEmpty(),
                 qPrintable(linesThatCouldBeAPage(connecting).join(u" | "_s)));

        // Nothing was fetched for any of it.
        QCOMPARE(elsewhere.asked.size(), 0);
        // And neither window had anything to say of what it was handed. (The first one's
        // view of the wall as a file went when the second window was made, once, and that window
        // said so twenty-three times: each window keeps what it was handed until the test is over.)
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBarsSmallButtonsDoWhatTheySay()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown = signedIn();
        session.show(shown);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        qkvm::ui::Look& look = *m_looks.back();

        // The theme: from one to the next, and the button says which it is and which comes.
        const QQuickItem* theme = theOne(window, u"themeButton"_s);
        QVERIFY(theme != nullptr);
        QCOMPARE(look.theme(), u"dark"_s);
        QCOMPARE(theme->property("hint").toString(), u"Dark theme (click for light theme)"_s);
        QCOMPARE(theme->property("symbol").toString(), u"moon"_s);
        clickOn(window, theme);
        QTRY_COMPARE(look.theme(), u"light"_s);
        QTRY_COMPARE(theme->property("hint").toString(), u"Light theme (click for system theme)"_s);
        QCOMPARE(theme->property("symbol").toString(), u"sun"_s);
        clickOn(window, theme);
        QTRY_COMPARE(look.theme(), u"system"_s);
        QCOMPARE(theme->property("symbol").toString(), u"system"_s);
        clickOn(window, theme);
        QTRY_COMPARE(look.theme(), u"dark"_s);

        // Over the whole screen and back: by the button, which then says the way back, and by F11.
        const QQuickItem* full = theOne(window, u"fullScreenButton"_s);
        QVERIFY(full != nullptr);
        QCOMPARE(full->property("hint").toString(), u"Fullscreen"_s);
        QCOMPARE(full->property("symbol").toString(), u"maximize"_s);
        QVERIFY(window->visibility() != QWindow::FullScreen);
        clickOn(window, full);
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        QTRY_COMPARE(full->property("hint").toString(), u"Leave fullscreen"_s);
        QCOMPARE(full->property("symbol").toString(), u"minimize"_s);
        clickOn(window, full);
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        QTest::keyClick(window, Qt::Key_F11);
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        // With a machine shown large too, where the bar is not there.
        wall.enlarge(qkvm::ui::pretendWall()[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedName"_s) != nullptr);
        QTest::keyClick(window, Qt::Key_F11);
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        wall.closeEnlarged();
        // A window that filled the desktop goes back to filling the desktop.
        window->showMaximized();
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        QTest::keyClick(window, Qt::Key_F11);
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        QTest::keyClick(window, Qt::Key_F11);
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        window->showNormal();
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);

        // "Add KVMs" leads to the QKVM's page, where machines are added: for whoever manages it,
        // and not there for anybody else.
        const QSignalSpy pageAsked(&session, &qkvm::ui::SessionView::pageAsked);
        const QQuickItem* add = theOne(window, u"addButton"_s);
        QVERIFY(add != nullptr);
        QTRY_VERIFY(add->isVisible());
        QCOMPARE(add->property("hint").toString(), u"Add KVMs on the qKVM page, in your browser"_s);
        clickOn(window, add);
        QTRY_COMPARE(pageAsked.count(), 1);
        shown.manages = false;
        session.show(shown);
        QTRY_VERIFY(!add->isVisible());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aCardNamedByItsAddressSaysTheAddressOnce()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        // A KVM that was added without a name, and whose own says nothing, is called by where it is.
        qkvm::ui::WallTile unnamed;
        unnamed.key = u"k1"_s;
        unnamed.name = u"192.168.1.20"_s;
        unnamed.host = u"192.168.1.20"_s;
        unnamed.state = u"online"_s;
        qkvm::ui::WallTile named = unnamed;
        named.key = u"k2"_s;
        named.name = u"NAS"_s;
        named.host = u"192.168.1.22"_s;
        wall.machines()->setTiles({unnamed, named});
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 2);
        const QQuickItem* first = inCard(cardOf(window->contentItem(), u"k1"_s), u"cardHost"_s);
        const QQuickItem* second = inCard(cardOf(window->contentItem(), u"k2"_s), u"cardHost"_s);
        QVERIFY(first != nullptr && second != nullptr);
        QCOMPARE(first->property("text").toString(), QString());
        QCOMPARE(second->property("text").toString(), u"192.168.1.22"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aMachineOnItsWayIsWaitedForWithoutASignOfTrouble()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        // A KVM that was told to restart and is away, as its card is then; and one that is offline.
        qkvm::ui::WallTile restarting;
        restarting.key = u"k1"_s;
        restarting.name = u"NAS"_s;
        restarting.host = u"192.168.1.22"_s;
        restarting.state = u"connecting"_s;
        restarting.label = u"Rebooting…"_s;
        restarting.title = u"Rebooting…"_s;
        restarting.text = u"Back in about a minute."_s;
        qkvm::ui::WallTile offline = restarting;
        offline.key = u"k2"_s;
        offline.name = u"Old server"_s;
        offline.state = u"offline"_s;
        offline.label = u"Offline"_s;
        offline.title = u"Offline"_s;
        offline.text = u"Device is unreachable"_s;
        wall.machines()->setTiles({restarting, offline});
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 2);
        QVERIFY(saying(window->contentItem(), u"Rebooting…"_s) != nullptr);
        QVERIFY(saying(window->contentItem(), u"Back in about a minute."_s) != nullptr);
        // The one that is waited for turns a ring; the one in trouble shows its sign, and no ring.
        // One of each on this wall: the waiting card has no sign of trouble beside its ring.
        QTRY_COMPARE(seenNamed(window, u"veilSpinner"_s), 1);
        QCOMPARE(seenNamed(window, u"veilSign"_s), 1);
        const QQuickItem* ring = nullptr;
        QList<const QQuickItem*> rings;
        named(window->contentItem(), u"veilSpinner"_s, rings);
        for (const QQuickItem* one : std::as_const(rings)) {
            if (one->isVisible()) {
                ring = one;
            }
        }
        QVERIFY(ring != nullptr);
        // It is the first card's: left of the second card.
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(ring->mapToScene(QPointF(0, 0)).x() < scrolled->width() / 2);
        keepPictureOf(window, u"wall-restarting"_s);
        // Counted with neither the live nor the troubled: only under "All".
        QCOMPARE(wall.machines()->all(), 2);
        QCOMPARE(wall.machines()->online(), 0);
        QCOMPARE(wall.machines()->attention(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void machinesArePickedOnTheWallForSomethingDoneToSeveral()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QSignalSpy portAsked(&actions, &qkvm::ui::ActionsView::portAsked);
        const QQuickItem* select = theOne(window, u"selectButton"_s);
        const QQuickItem* bulk = theOne(window, u"bulkBar"_s);
        QVERIFY(select != nullptr);
        QVERIFY(bulk != nullptr);
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        const auto onCard = [&](int column, int row) {
            const qreal width = scrolled->property("cardWidth").toReal();
            const qreal gap = scrolled->property("gap").toReal();
            return scrolled
                ->mapToScene(QPointF((scrolled->width() - scrolled->property("gridWidth").toReal()) / 2 +
                                         column * (width + gap) + width / 2,
                                     scrolled->property("gridTop").toReal() +
                                         row * (scrolled->property("cardHeight").toReal() + gap) + 60))
                .toPoint();
        };

        // Whoever may only look picks nothing: no button for it, and S does nothing.
        QVERIFY(!select->isVisible());
        QVERIFY(!bulk->isVisible());
        QTest::keyClick(window, Qt::Key_S);
        QTest::qWait(100);
        QVERIFY(!wall.selecting());

        // Whoever may act: the bar's button begins it, and the bar at the foot of the wall is there.
        actions.setAllowed(true);
        QTRY_VERIFY(select->isVisible());
        const qreal cardBefore = scrolled->property("cardWidth").toReal();
        clickOn(window, select);
        QTRY_VERIFY(wall.selecting());
        QTRY_VERIFY(bulk->isVisible());
        QVERIFY(says(bulk, u"0 selected"_s));
        QVERIFY(says(bulk, u"Select all"_s));
        QVERIFY(says(bulk, u"Clear"_s));
        // Whole, inside the window, in its middle.
        QVERIFY(bulk->mapToScene(QPointF(0, bulk->height())).y() <= window->height());
        QVERIFY(qAbs(bulk->mapToScene(QPointF(bulk->width() / 2, 0)).x() - window->width() / 2.0) < 2);
        // Every card shows whether it is picked; and the cards make room for the bar.
        QTRY_COMPARE(seenNamed(window, u"cardTick"_s), 15);
        QVERIFY(scrolled->property("cardWidth").toReal() <= cardBefore);
        // (This wall is longer than the window: scrolled to its end, its last card is above the bar.)
        auto* toScroll = window->findChild<QQuickItem*>(u"wall"_s);
        const qreal end = scrolled->property("contentHeight").toReal() - scrolled->height();
        QVERIFY(end > 0);
        toScroll->setProperty("contentY", end);
        const qreal lastCardEnds =
            scrolled->property("gridTop").toReal() + scrolled->property("gridHeight").toReal() - end;
        QVERIFY(scrolled->mapToScene(QPointF(0, lastCardEnds)).y() <= bulk->mapToScene(QPointF(0, 0)).y());
        toScroll->setProperty("contentY", 0);

        // A click on a card picks it, and a second lets go of it.
        clickOn(window, onCard(1, 0));
        QTRY_COMPARE(wall.pickedMachines(), QStringList{tiles[1].key});
        QTRY_VERIFY(says(bulk, u"1 selected"_s));
        clickOn(window, onCard(1, 0));
        QTRY_COMPARE(wall.picked(), 0);
        // Nothing on a card does anything else meanwhile: a click on one of its computers' numbers
        // picks the card, and no way into a menu or a console is under the pointer.
        QList<const QQuickItem*> ports;
        named(window->contentItem(), u"port"_s, ports);
        QVERIFY(ports.size() >= 3);
        clickOn(window, ports[2]);
        QTRY_COMPARE(wall.pickedMachines(), QStringList{tiles[5].key});
        QCOMPARE(portAsked.count(), 0);
        QTest::mouseMove(window, onCard(2, 0));
        QTest::qWait(150);
        QCOMPARE(seenNamed(window, u"cardActions"_s), 0);
        QCOMPARE(seenNamed(window, u"cardConsole"_s), 0);
        // A double click does not show the machine large.
        QTest::mouseDClick(window, Qt::LeftButton, {}, onCard(2, 0));
        QTest::qWait(150);
        QCOMPARE(wall.enlarged(), QString());
        QVERIFY(wall.selecting());
        keepPictureOf(window, u"wall-selecting"_s);

        // With the keyboard on a card: Space and Return pick it.
        wall.pickNone();
        clickOn(window, onCard(3, 0));
        QTRY_COMPARE(wall.pickedMachines(), QStringList{tiles[3].key});
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(wall.picked(), 0);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(wall.pickedMachines(), QStringList{tiles[3].key});

        // All that the bar leaves, and none.
        wall.shown()->setShow(u"attention"_s);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 2);
        clickOn(window, theOne(window, u"pickAll"_s));
        QTRY_COMPARE(wall.picked(), 3);
        QTRY_VERIFY(says(bulk, u"3 selected"_s));
        clickOn(window, theOne(window, u"pickNone"_s));
        QTRY_COMPARE(wall.picked(), 0);
        QVERIFY(!theOne(window, u"pickNone"_s)->isEnabled());
        wall.shown()->setShow(u"all"_s);

        // The way out: the bar's cross, Esc, the button, and S.
        clickOn(window, theOne(window, u"pickDone"_s));
        QTRY_VERIFY(!wall.selecting());
        QTRY_VERIFY(!bulk->isVisible());
        QCOMPARE(seenNamed(window, u"cardTick"_s), 0);
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(wall.selecting());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!wall.selecting());
        clickOn(window, select);
        QTRY_VERIFY(wall.selecting());
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(!wall.selecting());

        // "/" goes to "Find a KVM", and a letter typed there is a letter.
        QTest::keyClick(window, Qt::Key_Slash);
        const QQuickItem* find = theOne(window, u"findAKvm"_s);
        QVERIFY(find != nullptr);
        QTRY_VERIFY(find->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_S);
        QTest::qWait(100);
        QVERIFY(!wall.selecting());
        QCOMPARE(wall.shown()->find(), u"s"_s);
        wall.shown()->setFind(QString());

        // While one of the bar's menus is open the wall's keys rest too: S picks nothing, and Esc
        // closes the menu and does not end a picking that is under way.
        QObject* viewMenu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(viewMenu != nullptr);
        clickOn(window, theOne(window, u"viewButton"_s));
        QTRY_VERIFY(viewMenu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_S);
        QTest::qWait(100);
        QVERIFY(!wall.selecting());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!viewMenu->property("opened").toBool());
        wall.setSelecting(true);
        clickOn(window, theOne(window, u"viewButton"_s));
        QTRY_VERIFY(viewMenu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!viewMenu->property("opened").toBool());
        QTest::qWait(100);
        QVERIFY(wall.selecting());
        wall.setSelecting(false);

        // While a machine is shown large, the wall's keys are not that view's: S picks nothing, and
        // the wall is as it was when the machine is closed again.
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedName"_s) != nullptr);
        QTest::keyClick(window, Qt::Key_S);
        QTest::qWait(100);
        QVERIFY(!wall.selecting());
        QCOMPARE(wall.enlarged(), tiles[1].key);
        wall.closeEnlarged();
        QTRY_VERIFY(theOne(window, u"enlargedName"_s) == nullptr);

        // While a box is open the wall's keys rest: S typed at a dialog picks nothing.
        QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject*>(u"aboutBox"_s), "open"));
        QTRY_VERIFY(window->findChild<QObject*>(u"aboutBox"_s)->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_S);
        QTest::qWait(100);
        QVERIFY(!wall.selecting());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatIsDoneToThePickedMachinesIsAskedForAtTheFootOfTheWall()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const QSignalSpy pickedAsked(&actions, &qkvm::ui::ActionsView::pickedAsked);
        wall.setSelecting(true);
        const QQuickItem* bulk = theOne(window, u"bulkBar"_s);
        const QQuickItem* run = theOne(window, u"pickRun"_s);
        QVERIFY(bulk != nullptr);
        QVERIFY(run != nullptr);
        QTRY_VERIFY(bulk->isVisible());
        QVERIFY(says(run, u"Run action…"_s));
        // Nothing is picked: there is nothing to do it to.
        QVERIFY(!run->isEnabled());
        clickOn(window, run);
        QTest::qWait(100);
        QCOMPARE(pickedAsked.count(), 0);

        // Two are: the program is asked for what can be done to them.
        wall.togglePicked(tiles[0].key);
        wall.togglePicked(tiles[2].key);
        QTRY_VERIFY(run->isEnabled());
        keepPictureOf(window, u"wall-selecting-two"_s);
        clickOn(window, run);
        QTRY_COMPARE(pickedAsked.count(), 1);
        // With the keyboard as well.
        QVERIFY(tabTo(window->contentItem(), u"pickRun"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(pickedAsked.count(), 2);
        // The bar has room for it in the narrowest window there is.
        window->setWidth(window->minimumWidth());
        QTRY_VERIFY(isInside(window, bulk));

        // Their menu is a machine's menu: what it is of, what choosing does, and the rows.
        actions.showMenu(qkvm::ui::shown(menuOfSeveral()));
        QObject* menu = window->findChild<QObject*>(u"machineMenu"_s);
        QVERIFY(menu != nullptr);
        QTRY_VERIFY(menu->property("opened").toBool());
        for (const QString& text :
             {u"3 KVMs selected"_s,
              u"The action you choose runs on every selected KVM. You are asked to confirm first."_s, u"Wake screen"_s,
              u"Lock it"_s, u"Power on (servers)"_s, u"Shut down (servers)"_s, u"Press power button"_s}) {
            QVERIFY2(saying(window->contentItem(), text) != nullptr, qPrintable(text));
        }
        keepPictureOf(window, u"menu-of-several"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatIsDoneToSeveralIsShownAsItGoes()
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
        QObject* box = window->findChild<QObject*>(u"bulkProgress"_s);
        QVERIFY(box != nullptr);
        QVERIFY(!box->property("opened").toBool());
        const QSignalSpy stopAsked(&actions, &qkvm::ui::ActionsView::runStopAsked);
        const QSignalSpy retryAsked(&actions, &qkvm::ui::ActionsView::runRetryAsked);
        const QSignalSpy closeAsked(&actions, &qkvm::ui::ActionsView::runCloseAsked);

        // Under way: how far it is, and each machine with how it went so far.
        QVariantMap run =
            aRun(u"1 of 3 done…"_s, true, QString(),
                 {runRow(u"NAS"_s, u"ok"_s, u"power button held"_s), runRow(u"Build box"_s, QString(), u"Sending…"_s),
                  runRow(u"Rack server"_s, QString(), u"Waiting…"_s)});
        actions.showRun(run);
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_COMPARE(seenNamed(window, u"runRow"_s), 3);
        for (const QString& text :
             {u"Hold power button · 3 KVMs"_s, u"1 of 3 done…"_s, u"NAS"_s, u"power button held"_s, u"Build box"_s,
              u"Sending…"_s, u"Rack server"_s, u"Waiting…"_s}) {
            QVERIFY2(saying(window->contentItem(), text) != nullptr, qPrintable(text));
        }
        // While it runs it can be stopped, and nothing else.
        const QQuickItem* stop = theOne(window, u"runStop"_s);
        QVERIFY(stop != nullptr);
        QVERIFY(says(stop, u"Stop"_s));
        QVERIFY(stop->isEnabled());
        // That is where the keyboard is.
        QTRY_VERIFY(stop->hasActiveFocus());
        QCOMPARE(seenNamed(window, u"runStop"_s), 1);
        QCOMPARE(seenNamed(window, u"runClose"_s), 0);
        QCOMPARE(seenNamed(window, u"runAgain"_s), 0);
        keepPictureOf(window, u"run-under-way"_s);
        clickOn(window, stop);
        QTRY_COMPARE(stopAsked.count(), 1);
        // Stopping: what is on its way is waited for, and "stop" is not said twice.
        run[u"stopping"_s] = true;
        run[u"rows"_s] = QVariantList{runRow(u"NAS"_s, u"ok"_s, u"power button held"_s),
                                      runRow(u"Build box"_s, QString(), u"Sending…"_s),
                                      runRow(u"Rack server"_s, u"skipped"_s, u"Not sent"_s)};
        actions.showRun(run);
        QTRY_VERIFY(!stop->isEnabled());
        QTRY_VERIFY(saying(window->contentItem(), u"Not sent"_s) != nullptr);
        clickOn(window, stop);
        QTest::qWait(100);
        QCOMPARE(stopAsked.count(), 1);
        QVERIFY(box->property("opened").toBool());

        // Over: how it went, what can be sent once more, and the way out.
        actions.showRun(aRun(u"1 succeeded, 1 failed, 1 not sent."_s, false, u"Retry failed, send the rest"_s,
                             {runRow(u"NAS"_s, u"ok"_s, u"power button held"_s),
                              runRow(u"Build box"_s, u"bad"_s, u"Build box is not reachable."_s),
                              runRow(u"Rack server"_s, u"skipped"_s, u"Not sent"_s)}));
        QTRY_COMPARE(seenNamed(window, u"runStop"_s), 0);
        QTRY_COMPARE(seenNamed(window, u"runAgain"_s), 1);
        QCOMPARE(seenNamed(window, u"runClose"_s), 1);
        QVERIFY(saying(window->contentItem(), u"1 succeeded, 1 failed, 1 not sent."_s) != nullptr);
        QVERIFY(saying(window->contentItem(), u"Build box is not reachable."_s) != nullptr);
        QVERIFY(says(theOne(window, u"runAgain"_s), u"Retry failed, send the rest"_s));
        QVERIFY(says(theOne(window, u"runClose"_s), u"Close"_s));
        // The keyboard has gone from what is no longer there to the way out.
        QTRY_VERIFY(theOne(window, u"runClose"_s)->hasActiveFocus());
        keepPictureOf(window, u"run-over"_s);
        clickOn(window, theOne(window, u"runAgain"_s));
        QTRY_COMPARE(retryAsked.count(), 1);
        clickOn(window, theOne(window, u"runClose"_s));
        QTRY_COMPARE(closeAsked.count(), 1);
        // The box stays until the program takes it away; and taken away, it asks for nothing more.
        QVERIFY(box->property("opened").toBool());
        actions.showRun({});
        QTRY_VERIFY(!box->property("opened").toBool());
        QTest::qWait(100);
        QCOMPARE(closeAsked.count(), 1);
        QCOMPARE(stopAsked.count(), 1);

        // Every machine did it: nothing is offered again.
        actions.showRun(aRun(u"Done on all 3 KVMs."_s, false, QString(),
                             {runRow(u"NAS"_s, u"ok"_s, u"power button held"_s),
                              runRow(u"Build box"_s, u"ok"_s, u"power button held"_s),
                              runRow(u"Rack server"_s, u"unknown"_s, u"No answer. It may have been done."_s)}));
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_COMPARE(seenNamed(window, u"runClose"_s), 1);
        QCOMPARE(seenNamed(window, u"runAgain"_s), 0);
        QCOMPARE(seenNamed(window, u"runStop"_s), 0);
        QTRY_VERIFY(saying(window->contentItem(), u"No answer. It may have been done."_s) != nullptr);
        // Opened on a run that is over, the keyboard is at the way out, and Space takes it.
        QTRY_VERIFY(theOne(window, u"runClose"_s)->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(closeAsked.count(), 2);
        actions.showRun({});
        QTRY_VERIFY(!box->property("opened").toBool());

        // Closed by hand while it runs, with Esc: the program is told, for which that is a stop.
        actions.showRun(run);
        QTRY_VERIFY(box->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(closeAsked.count(), 3);
        actions.showRun({});
        QTRY_VERIFY(!box->property("opened").toBool());
        QTest::qWait(100);
        QCOMPARE(closeAsked.count(), 3);
        QCOMPARE(stopAsked.count(), 1);

        // Opened on a run that is over and has something to send once more: the keyboard is at
        // the way out all the same, and not at what would send it.
        actions.showRun(aRun(u"2 succeeded, 1 failed."_s, false, u"Retry failed"_s,
                             {runRow(u"NAS"_s, u"ok"_s, u"power button held"_s),
                              runRow(u"Build box"_s, u"bad"_s, u"Build box is not reachable."_s),
                              runRow(u"Rack server"_s, u"ok"_s, u"power button held"_s)}));
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_COMPARE(seenNamed(window, u"runAgain"_s), 1);
        QTRY_VERIFY(theOne(window, u"runClose"_s)->hasActiveFocus());
        QVERIFY(!theOne(window, u"runAgain"_s)->hasActiveFocus());
        actions.showRun({});
        QTRY_VERIFY(!box->property("opened").toBool());

        // Many machines, in the smallest window there is: the list scrolls, and the box with its
        // way out stays whole inside the window.
        QVariantList many;
        for (int number = 1; number <= 40; ++number) {
            many.append(runRow(u"Node %1"_s.arg(number), number % 3 == 0 ? u"bad"_s : u"ok"_s,
                               number % 3 == 0 ? u"Node %1 answered with something that is not an answer."_s.arg(number)
                                               : u"power button held"_s));
        }
        window->setWidth(window->minimumWidth());
        window->setHeight(window->minimumHeight());
        actions.showRun(aRun(u"27 succeeded, 13 failed."_s, false, u"Retry failed"_s, many));
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_VERIFY(theOne(window, u"runClose"_s) != nullptr);
        QTRY_VERIFY(isInside(window, theOne(window, u"runClose"_s)));
        QVERIFY(isInside(window, theOne(window, u"runAgain"_s)));
        QVERIFY(isInside(window, theOne(window, u"runSummary"_s)));
        QVERIFY(seenNamed(window, u"runRow"_s) >= 2);
        keepPictureOf(window, u"run-many-small"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aWallThatIsOnlyShownHasNoBarButASmallOneOfItsOwn()
    {
        qkvm::ui::AboutModel about;
        // (A group's turn is over after a quarter of a second here.)
        qkvm::ui::WallView wall(nullptr, 250);
        const QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        wall.setColumns(1);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        auto* small = window->findChild<QQuickItem*>(u"displayBar"_s);
        QVERIFY(small != nullptr);
        // It stays for a moment after the pointer last moved: a short one here.
        small->setProperty("staysMs", 400);
        const QQuickItem* viewButton = theOne(window, u"viewButton"_s);
        QVERIFY(viewButton != nullptr);
        QVERIFY(viewButton->isVisible());
        QVERIFY(!small->isVisible());
        QCOMPARE(scrolled->property("columns").toInt(), 1);
        const qreal wallTop = scrolled->mapToScene(QPointF(0, 0)).y();
        QVERIFY(wallTop > 30);

        // The small bar shows as display mode begins, which is where the way out is said: there
        // and then, whether or not a pointer moves (a screen nobody sits at has none). And it gets
        // out of the way.
        wall.setDisplaying(true);
        QCOMPARE(small->property("awake").toBool(), true);
        QTRY_COMPARE(small->opacity(), 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(small->opacity(), 0.0, 3000);
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(!wall.displaying());
        QTRY_VERIFY(viewButton->isVisible());

        // From the View menu's last row, which names its key.
        QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(menu != nullptr);
        clickOn(window, viewButton);
        QTRY_VERIFY(menu->property("opened").toBool());
        const QQuickItem* row = rowOf(menu, u"Display mode"_s);
        QVERIFY(row != nullptr);
        QVERIFY(rowOf(menu, u"D"_s) != nullptr);
        keepPictureOf(window, u"menu-view-display"_s);
        clickOn(window, row);
        QTRY_VERIFY(wall.displaying());
        QTRY_VERIFY(!menu->property("opened").toBool());
        // The bar is gone, the wall begins at the window's top, and the cards are as large as lets
        // all of them be seen, whatever number of columns was chosen.
        QTRY_VERIFY(!viewButton->isVisible());
        QTRY_COMPARE(scrolled->mapToScene(QPointF(0, 0)).y(), 0.0);
        QTRY_VERIFY(scrolled->property("columns").toInt() > 1);
        QCOMPARE(wall.columns(), 1);
        // Its own small bar is there, at the top right, and out of the way until the pointer moves.
        // The keyboard, which was at the View button, is not left at a button nobody sees: Space
        // does nothing.
        QVERIFY(small->isVisible());
        // (It shows for a moment as display mode begins: that is where the way out is.)
        QTRY_COMPARE(small->opacity(), 1.0);
        QTRY_COMPARE(small->opacity(), 0.0);
        QTRY_VERIFY(window->activeFocusItem() == nullptr || window->activeFocusItem()->objectName() == u"wall"_s);
        QTest::keyClick(window, Qt::Key_Space);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(100);
        QVERIFY(!wall.rotating());
        QVERIFY(wall.displaying());
        QVERIFY(small->mapToScene(QPointF(small->width(), 0)).x() <= window->width());
        QVERIFY(small->mapToScene(QPointF(small->width(), 0)).x() > window->width() - 40);
        QVERIFY(small->mapToScene(QPointF(0, 0)).y() < 40);
        keepPictureOf(window, u"display"_s);
        QTest::mouseMove(window, QPoint(300, 300));
        QTest::mouseMove(window, QPoint(320, 310));
        QTRY_COMPARE(small->opacity(), 1.0);
        for (const QString& text : {u"All groups"_s, u"Rotate groups"_s, u"Exit (Esc)"_s}) {
            QVERIFY2(saying(small, text) != nullptr, qPrintable(text));
        }
        keepPictureOf(window, u"display-awake"_s);
        // And gets out of the way again by itself.
        QTRY_COMPARE_WITH_TIMEOUT(small->opacity(), 0.0, 3000);

        // The groups take turns where that is ticked, and the small bar says whose turn it is.
        const QQuickItem* rotate = theOne(window, u"displayRotate"_s);
        QVERIFY(rotate != nullptr);
        QVERIFY(!wall.rotating());
        QTest::mouseMove(window, QPoint(300, 320));
        QTest::mouseMove(window, QPoint(310, 330));
        QTRY_COMPARE(small->opacity(), 1.0);
        clickOn(window, rotate);
        QTRY_VERIFY(wall.rotating());
        QTRY_COMPARE(wall.shown()->group(), u"Office"_s);
        QTRY_VERIFY(saying(small, u"Office"_s) != nullptr);
        QTRY_COMPARE(wall.shown()->group(), u"Rack A"_s);
        QTRY_VERIFY(saying(small, u"Rack A"_s) != nullptr);
        // While a box lies over the wall, they wait.
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(window->findChild<QObject*>(u"machineMenu"_s)->property("opened").toBool());
        QTRY_VERIFY(wall.covered());
        const QString stays = wall.shown()->group();
        QTest::qWait(700);
        QCOMPARE(wall.shown()->group(), stays);
        actions.showMenu({});
        QTRY_VERIFY(!wall.covered());
        QTRY_VERIFY(wall.shown()->group() != stays);
        // Unticked with the keyboard, which also keeps the small bar there while it is in it.
        // (From the wall, where the keyboard is while nobody has clicked into the small bar: a click
        // on the tick does not keep the bar there, a Tab to it does.)
        QTRY_COMPARE_WITH_TIMEOUT(small->opacity(), 0.0, 3000);
        window->findChild<QQuickItem*>(u"wall"_s)->forceActiveFocus();
        QVERIFY(tabTo(window->contentItem(), u"displayRotate"_s));
        QTRY_COMPARE(small->opacity(), 1.0);
        QTest::qWait(700);
        QCOMPARE(small->opacity(), 1.0);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!wall.rotating());

        // The ways out: its button, Esc, and D, which is also the way in.
        const QQuickItem* leave = theOne(window, u"displayExit"_s);
        QVERIFY(leave != nullptr);
        clickOn(window, leave);
        QTRY_VERIFY(!wall.displaying());
        QTRY_VERIFY(viewButton->isVisible());
        QVERIFY(!small->isVisible());
        QTRY_COMPARE(scrolled->property("columns").toInt(), 1);
        QTRY_COMPARE(scrolled->mapToScene(QPointF(0, 0)).y(), wallTop);
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(wall.displaying());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!wall.displaying());
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(wall.displaying());
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(!wall.displaying());

        // While machines are being picked on a wall that is only shown, Esc ends the picking first.
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(wall.displaying());
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(wall.selecting());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!wall.selecting());
        QVERIFY(wall.displaying());
        // "/" has no line to go to here: nothing is typed anywhere, and nothing complains.
        QTest::keyClick(window, Qt::Key_Slash);
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(wall.selecting());
        QCOMPARE(wall.shown()->find(), QString());
        wall.setSelecting(false);
        // A machine that is shown large has the window: the small bar is not over it, and D is not
        // the wall's key meanwhile.
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedName"_s) != nullptr);
        QVERIFY(!small->isVisible());
        QTest::keyClick(window, Qt::Key_D);
        QTest::qWait(100);
        QVERIFY(wall.displaying());
        wall.closeEnlarged();
        QTRY_VERIFY(small->isVisible());

        // A wall with one group, or none, has nothing to take turns.
        QList<qkvm::ui::WallTile> oneGroup = tiles;
        for (qkvm::ui::WallTile& tile : oneGroup) {
            tile.group = u"Office"_s;
        }
        wall.machines()->setTiles(oneGroup);
        QTest::mouseMove(window, QPoint(300, 340));
        QTest::mouseMove(window, QPoint(310, 350));
        QTRY_COMPARE(small->opacity(), 1.0);
        QVERIFY(!rotate->isVisible());
        QVERIFY(saying(small, u"Exit (Esc)"_s) != nullptr);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // The bar's filters as the page has them: All and Online always, No signal and Attention only
    // while something is in them or one of them is what is chosen; and what is chosen, pressed
    // again, gives way to All.
    void theFiltersShowWhatHasSomethingInItAndASecondPressGivesAll()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        // Five machines, all of them online.
        QList<qkvm::ui::WallTile> tiles = fiveMachines();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        const auto filter = [&](const QString& key) { return theOne(window, u"filter-"_s + key); };
        for (const QString& key : {u"all"_s, u"online"_s, u"no_signal"_s, u"attention"_s}) {
            QVERIFY2(filter(key) != nullptr, qPrintable(key));
        }
        QTRY_VERIFY(isSeen(filter(u"all"_s)));
        QVERIFY(isSeen(filter(u"online"_s)));
        QVERIFY(!isSeen(filter(u"no_signal"_s)));
        QVERIFY(!isSeen(filter(u"attention"_s)));
        keepPictureOf(window, u"filters-nothing-amiss"_s);

        // Chosen while nothing is in it (it was chosen before, and is remembered): there, with its
        // nought, so that what narrows the wall can be seen and pressed again.
        wall.shown()->setShow(u"no_signal"_s);
        QTRY_VERIFY(isSeen(filter(u"no_signal"_s)));
        QVERIFY(!isSeen(filter(u"attention"_s)));
        QCOMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 0);
        // Pressed again: All.
        clickOn(window, filter(u"no_signal"_s));
        QTRY_COMPARE(wall.shown()->show(), u"all"_s);
        QTRY_VERIFY(!isSeen(filter(u"no_signal"_s)));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 5);
        // And by the keyboard: the keyboard goes to All, and is not left on what went.
        wall.shown()->setShow(u"no_signal"_s);
        QTRY_VERIFY(isSeen(filter(u"no_signal"_s)));
        QVERIFY(tabTo(window->contentItem(), u"filter-no_signal"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(wall.shown()->show(), u"all"_s);
        QTRY_VERIFY(!isSeen(filter(u"no_signal"_s)));
        QTRY_VERIFY(filter(u"all"_s)->hasActiveFocus());

        // Online, and Online again.
        clickOn(window, filter(u"online"_s));
        QTRY_COMPARE(wall.shown()->show(), u"online"_s);
        clickOn(window, filter(u"online"_s));
        QTRY_COMPARE(wall.shown()->show(), u"all"_s);
        // All, pressed while it is chosen, stays.
        clickOn(window, filter(u"all"_s));
        QTest::qWait(50);
        QCOMPARE(wall.shown()->show(), u"all"_s);

        // A machine with no signal, and one that wants attention: theirs are there.
        tiles[1].state = u"no_signal"_s;
        tiles[1].label = u"No signal"_s;
        tiles[2].state = u"offline"_s;
        tiles[2].label = u"Offline"_s;
        wall.machines()->setTiles(tiles);
        QTRY_VERIFY(isSeen(filter(u"no_signal"_s)));
        QTRY_VERIFY(isSeen(filter(u"attention"_s)));
        clickOn(window, filter(u"attention"_s));
        QTRY_COMPARE(wall.shown()->show(), u"attention"_s);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 1);
        clickOn(window, filter(u"attention"_s));
        QTRY_COMPARE(wall.shown()->show(), u"all"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Esc in "Find a KVM" empties it and leaves it, as on the page: the wall is all there again,
    // and its keys are the wall's. Nothing else: while machines are picked, they still are.
    void escInFindEmptiesItAndLeavesIt()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QQuickItem* find = theOne(window, u"findAKvm"_s);
        QVERIFY(find != nullptr);

        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_VERIFY(find->hasActiveFocus());
        type(window, u"nas"_s);
        QTRY_COMPARE(wall.shown()->find(), u"nas"_s);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 1);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.shown()->find(), QString());
        QCOMPARE(find->property("text").toString(), QString());
        QVERIFY(!find->hasActiveFocus());
        QTRY_VERIFY(window->findChild<QQuickItem*>(u"wall"_s)->hasActiveFocus());
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        // The wall's keys are the wall's again: "/" goes back into it.
        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_VERIFY(find->hasActiveFocus());

        // While machines are picked: Esc there empties it, and the picking goes on.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!find->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(wall.selecting());
        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_VERIFY(find->hasActiveFocus());
        type(window, u"rack"_s);
        QTRY_COMPARE(wall.shown()->find(), u"rack"_s);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.shown()->find(), QString());
        QVERIFY(!find->hasActiveFocus());
        QVERIFY(wall.selecting());
        // And Esc once more, now that the wall has the keys, leaves the picking.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!wall.selecting());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // A wall that the bar's choices narrow to nothing says so as the page does, "No KVMs match",
    // and "Show all" gives back all of it: the filter, the group and what was looked for.
    void aWallNarrowedToNothingSaysNoKvmsMatchAndShowAllGivesItBack()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QQuickItem* noneMatch = theOne(window, u"noKvmsMatch"_s);
        QVERIFY(noneMatch != nullptr);
        QVERIFY(!isSeen(noneMatch));

        wall.shown()->setGroup(u"Rack A"_s);
        wall.shown()->setShow(u"online"_s);
        wall.shown()->setFind(u"no machine is called this"_s);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 0);
        QTRY_VERIFY(isSeen(noneMatch));
        QVERIFY(says(noneMatch, u"No KVMs match"_s));
        QVERIFY(!says(window->contentItem(), u"Nothing to show"_s));
        const QQuickItem* showAll = theOne(window, u"showAll"_s);
        QVERIFY(showAll != nullptr && isSeen(showAll));
        QVERIFY(says(showAll, u"Show all"_s));
        // The page's sign, 14 px over the words (its gap and the sign's own margin).
        const QQuickItem* sign = theOne(window, u"noMatchSign"_s);
        const QQuickItem* words = saying(noneMatch, u"No KVMs match"_s);
        QVERIFY(sign != nullptr && words != nullptr);
        QCOMPARE(words->mapToScene(QPointF(0, 0)).y() - sign->mapToScene(QPointF(0, sign->height())).y(), 14.0);
        keepPictureOf(window, u"wall-no-match"_s);
        clickOn(window, showAll);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        QCOMPARE(wall.shown()->find(), QString());
        QCOMPARE(wall.shown()->group(), QString());
        QCOMPARE(wall.shown()->show(), u"all"_s);
        QCOMPARE(theOne(window, u"findAKvm"_s)->property("text").toString(), QString());
        QVERIFY(!isSeen(noneMatch));
        // The keyboard is the wall's, and not left on the button that went; by the keyboard too.
        const QQuickItem* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QTRY_VERIFY(scrolled->hasActiveFocus());
        wall.shown()->setFind(u"no machine is called this"_s);
        QTRY_VERIFY(isSeen(noneMatch));
        QVERIFY(tabTo(window->contentItem(), u"showAll"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!isSeen(noneMatch));
        QTRY_VERIFY(scrolled->hasActiveFocus());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void anEmptyWallSaysSo()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 0);
        QVERIFY(says(window->contentItem(), u"Nothing to show"_s));
        keepPictureOf(window, u"wall-empty"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // --- this PC's own wall: KVMs that the program speaks to itself ---

    void thisPcsOwnWallWithNoKvmYetLeadsToTheFirst()
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

        // What the wall is for, in the place of "Nothing to show", and the way to the first KVM.
        QVERIFY(says(window->contentItem(), u"No KVMs on this wall yet"_s));
        QVERIFY(!says(window->contentItem(), u"Nothing to show"_s));
        keepPictureOf(window, u"own-wall-empty"_s);
        QObject* box = window->findChild<QObject*>(u"addKvm"_s);
        QVERIFY(box != nullptr);
        QVERIFY(!box->property("opened").toBool());
        const auto* first = window->findChild<QQuickItem*>(u"addFirstKvm"_s);
        QVERIFY(first != nullptr);
        clickOn(window, first);
        QTRY_VERIFY(box->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(box, "close"));
        QTRY_VERIFY(!box->property("opened").toBool());

        // With a KVM on the wall the hint is gone; and a wall that the bar narrows to nothing says
        // what any wall says then, as the page does.
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        QTRY_VERIFY(!says(window->contentItem(), u"No KVMs on this wall yet"_s));
        wall.shown()->setFind(u"no machine is called this"_s);
        QTRY_VERIFY(says(window->contentItem(), u"No KVMs match"_s));
        QVERIFY(!says(window->contentItem(), u"No KVMs on this wall yet"_s));

        // And a server's wall with nothing on it says nothing of adding here.
        wall.shown()->setFind(QString());
        wall.machines()->setTiles({});
        session.show(signedIn());
        QTRY_VERIFY(says(window->contentItem(), u"Nothing to show"_s));
        QVERIFY(!says(window->contentItem(), u"No KVMs on this wall yet"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // The bar's More, open, in both looks, in the smallest window, a middling one and a wide one:
    // whole inside the window, and what it holds as the page would show it in the bar.
    void theBarsMoreIsWholeInsideTheWindowInBothLooks_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<QString>("theme");
        for (const int width : {640, 800, 1280}) {
            for (const QString& theme : {u"dark"_s, u"light"_s}) {
                QTest::addRow("%d wide, %s", width, qPrintable(theme)) << width << theme;
            }
        }
    }

    void theBarsMoreIsWholeInsideTheWindowInBothLooks()
    {
        QFETCH(int, width);
        QFETCH(QString, theme);
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
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        window->resize(width, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        QVERIFY(whatGaveWayIsInMore(window));
        const auto* more = theOne(window, u"moreButton"_s);
        QVERIFY(more != nullptr);
        if (isSeen(more)) {
            clickOn(window, more);
            const QObject* menu = window->findChild<QObject*>(u"moreMenu"_s);
            QTRY_VERIFY(menu->property("opened").toBool());
            const auto* inside = menu->property("contentItem").value<QQuickItem*>();
            QVERIFY(isInside(window, inside->parentItem()));
        }
        keepPictureOf(window, u"bar-more-%1-%2"_s.arg(width).arg(theme));
        if (width > 640) {
            QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
            return;
        }
        // In the smallest window, each row of More does what the button it stands for does.
        QObject* menu = window->findChild<QObject*>(u"moreMenu"_s);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        const auto choose = [&](const QString& row) {
            clickOn(window, more);
            if (!QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 2000) ||
                rowOf(menu, row) == nullptr) {
                return false;
            }
            clickOn(window, rowOf(menu, row));
            return QTest::qWaitFor([&] { return !menu->property("opened").toBool(); }, 2000);
        };
        QVERIFY(choose(u"Rack A"_s));
        QTRY_COMPARE(wall.shown()->group(), u"Rack A"_s);
        QVERIFY(choose(u"All groups"_s));
        QTRY_COMPARE(wall.shown()->group(), QString());
        const QString before = m_looks.back()->theme();
        QVERIFY(choose(m_looks.back()->themeHint()));
        QTRY_VERIFY(m_looks.back()->theme() != before);
        QVERIFY(choose(u"Fullscreen"_s));
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        // (The whole screen is wide enough for every button of the bar: back by F11.)
        QTest::keyClick(window, Qt::Key_F11);
        QTRY_VERIFY(window->visibility() != QWindow::FullScreen);
        QTRY_COMPARE(window->width(), 640);
        QVERIFY(choose(u"Settings"_s));
        QObject* settings = window->findChild<QObject*>(u"settingsBox"_s);
        QTRY_VERIFY(settings->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theViewIsWholeInTheLowestWindowInBothLooks_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<QString>("theme");
        for (const int width : {640, 800, 1280}) {
            for (const QString& theme : {u"dark"_s, u"light"_s}) {
                QTest::addRow("%d wide, %s", width, qPrintable(theme)) << width << theme;
            }
        }
    }

    // The View as the page's panel (index.html, "view-panel"), for an admin, in a window as low as
    // a window gets: whole inside it without scrolling, every line of it with the room to say what
    // it says, and the list of a choice inside the window too. A pause is still said on the button.
    void theViewIsWholeInTheLowestWindowInBothLooks()
    {
        QFETCH(int, width);
        QFETCH(QString, theme);
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
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        window->resize(width, 400);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        QTRY_COMPARE(window->height(), 400);
        const auto* button = theOne(window, u"viewButton"_s);
        QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QVERIFY(button != nullptr && menu != nullptr);
        const bool inWords = says(button, u"View"_s);
        QCOMPARE(inWords, width == 1280);

        clickOn(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        const auto* inside = menu->property("flick").value<QQuickItem*>();
        QTRY_VERIFY(isInside(window, inside));
        QVERIFY(!inside->property("scrolls").toBool());
        for (const QString& line :
             {u"Order"_s, u"As arranged"_s, u"Columns"_s, u"Fit"_s, u"A new picture every"_s, u"1 s"_s,
              u"Arrange the tiles…"_s, u"Display mode"_s, u"D"_s, u"New window"_s, u"Ctrl+N"_s}) {
            const QQuickItem* said = rowOf(menu, line);
            QVERIFY2(said != nullptr, qPrintable(line));
            QVERIFY2(isInside(window, said), qPrintable(line));
            QVERIFY2(said->width() >= said->implicitWidth(), qPrintable(line));
        }
        // The page's icons: before what each choice says is chosen (".pick .icon"), and before
        // "Display mode" and "New window".
        for (const auto& [name, icon] :
             {std::pair{u"orderChoice"_s, u"list"_s}, std::pair{u"columnsChoice"_s, u"columns"_s},
              std::pair{u"paceChoice"_s, u"clock"_s}}) {
            const QQuickItem* choice = theOneSeen(window, name);
            QVERIFY2(choice != nullptr && drawsIcon(choice, icon), qPrintable(name));
            const QQuickItem* said = saying(choice, chosenIn(window, name));
            QVERIFY2(said != nullptr && said->x() >= 9 + 14, qPrintable(name));
        }
        QVERIFY(drawsIcon(rowOf(menu, u"Display mode"_s)->parentItem(), u"tv"_s));
        QVERIFY(drawsIcon(rowOf(menu, u"Arrange the tiles…"_s)->parentItem(), u"grid"_s));
        QVERIFY(rowOf(menu, u"Display mode"_s)->x() >= 9 + 14);
        QVERIFY(drawsIcon(rowOf(menu, u"New window"_s)->parentItem(), u"external"_s));
        keepPictureOf(window, u"view-%1-%2"_s.arg(width).arg(theme));
        // A choice's list, over the View and inside the window.
        clickOn(window, theOneSeen(window, u"paceChoice"_s));
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 0);
        QVERIFY(choicesCanBeRead(window));
        keepPictureOf(window, u"view-pace-%1-%2"_s.arg(width).arg(theme));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QVERIFY(menu->property("opened").toBool());

        // Paused: the button shows it, in words where it has words.
        QVERIFY(chooseIn(window, u"paceChoice"_s, u"Paused"_s));
        QTRY_COMPARE(wall.pace(), 0);
        QTRY_COMPARE(button->property("symbol").toString(), u"pause"_s);
        QVERIFY(button->property("noted").toBool());
        QCOMPARE(says(button, u"Paused"_s), inWords);
        QCOMPARE(chosenIn(window, u"paceChoice"_s), u"Paused"_s);
        keepPictureOf(window, u"view-paused-%1-%2"_s.arg(width).arg(theme));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(button->property("symbol").toString(), u"pause"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theAccountIsThePagesDropInBothLooks_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<QString>("theme");
        for (const int width : {640, 800, 1280}) {
            for (const QString& theme : {u"dark"_s, u"light"_s}) {
                QTest::addRow("%d wide, %s", width, qPrintable(theme)) << width << theme;
            }
        }
    }

    // The account as the page's drop (index.html, "me-panel"): who and where, a line, what is done
    // for this account and this PC, a line, and the way out. For an admin of a server's wall, with
    // another source and saved workspaces, in a window as low as a window gets: inside it, in that
    // order, every line with the room to say what it says, and the page's icons. From the keyboard
    // the keys are at its first part, and Esc gives them back to its button.
    void theAccountIsThePagesDropInBothLooks()
    {
        QFETCH(int, width);
        QFETCH(QString, theme);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        session.showSources(
            {QVariantMap{{u"value"_s, u"pc"_s}, {u"label"_s, u"This PC"_s}},
             QVariantMap{{u"value"_s, u"https://192.168.1.10:8787/"_s}, {u"label"_s, u"192.168.1.10:8787"_s}}},
            u"https://192.168.1.10:8787/"_s, true);
        QTemporaryDir folder;
        qkvm::core::Settings settings(folder.filePath(u"settings.json"_s));
        qkvm::ui::WorkspacesView workspaces(settings);
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(window->setProperty("workspaces", QVariant::fromValue(&workspaces)));
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        window->resize(width, 400);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        auto* button = window->findChild<QQuickItem*>(u"accountButton"_s);
        QObject* menu = window->findChild<QObject*>(u"accountMenu"_s);
        QVERIFY(button != nullptr && menu != nullptr);

        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(button->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_COMPARE(window->activeFocusItem()->objectName(), u"sourceChoice"_s);
        // (Where the keys are is seen: the drop was opened from the keyboard.)
        QVERIFY(window->activeFocusItem()->property("visualFocus").toBool());
        auto* inside = menu->property("flick").value<QQuickItem*>();
        QTRY_VERIFY(isInside(window, inside));
        const auto* content = inside->property("contentItem").value<QQuickItem*>();
        const QStringList lines{u"anna"_s,
                                u"on the qKVM at 192.168.1.10:8787"_s,
                                u"Source for this window"_s,
                                u"Browse all sources…"_s,
                                u"Saved workspaces…"_s,
                                u"Sign out"_s};
        for (const QString& line : lines) {
            const QQuickItem* said = rowOf(menu, line);
            QVERIFY2(said != nullptr, qPrintable(line));
            QVERIFY2(said->width() >= said->implicitWidth(), qPrintable(line));
        }
        // (Once laid out, with its first frame.)
        const auto inOrder = [&] {
            qreal above = -1;
            return std::ranges::all_of(lines, [&](const QString& line) {
                const qreal at = rowOf(menu, line)->mapToItem(content, QPointF(0, 0)).y();
                const bool below = at > above;
                above = at;
                return below;
            });
        };
        QTRY_VERIFY(inOrder());
        QCOMPARE(chosenIn(window, u"sourceChoice"_s), u"192.168.1.10:8787"_s);
        for (const auto& [row, icon] :
             {std::pair{u"browseSources"_s, u"search"_s}, std::pair{u"savedWorkspaces"_s, u"archive"_s},
              std::pair{u"signOut"_s, u"logOut"_s}}) {
            const QQuickItem* one = theOneSeen(window, row);
            QVERIFY2(one != nullptr && drawsIcon(one, icon), qPrintable(row));
        }
        keepPictureOf(window, u"account-%1-%2"_s.arg(width).arg(theme));
        // Where the window is too low for all of it (here, the room it leaves is made so), it
        // scrolls, and each part the keys come to, down and round to the first again, is in sight.
        QVERIFY(menu->setProperty("room", 150));
        QTRY_VERIFY(inside->property("scrolls").toBool());
        const auto inSight = [&](const QQuickItem* part) {
            return inside->mapRectToScene(QRectF(0, 0, inside->width(), inside->height()))
                .adjusted(-1, -1, 1, 1)
                .contains(part->mapRectToScene(QRectF(0, 0, part->width(), part->height())));
        };
        for (const QString& words :
             {u"Browse all sources…"_s, u"Saved workspaces…"_s, u"Sign out"_s, u"192.168.1.10:8787"_s}) {
            QTest::keyClick(window, Qt::Key_Down);
            QTRY_VERIFY2(says(window->activeFocusItem(), words), qPrintable(words));
            QTRY_VERIFY2(inSight(window->activeFocusItem()), qPrintable(words));
            QVERIFY(window->activeFocusItem()->property("visualFocus").toBool());
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTRY_VERIFY(button->hasActiveFocus());
        QVERIFY(button->property("visualFocus").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theProgramsWindowsCarryItsMark()
    {
        // What the title bar and the taskbar show for the program: said once, for every window.
        const QIcon icon = QGuiApplication::windowIcon();
        QVERIFY(!icon.isNull());
        for (const int size : {16, 24, 32, 48, 256}) {
            QVERIFY2(icon.availableSizes().contains(QSize(size, size)), qPrintable(QString::number(size)));
        }
        const QImage mark = icon.pixmap(32, 32).toImage();
        QCOMPARE(mark.size(), QSize(32, 32));
        // The official white mark on its blue gradient.
        QCOMPARE(mark.pixelColor(10, 10), QColor(Qt::white));
        QVERIFY(mark.pixelColor(16, 16).blue() > mark.pixelColor(16, 16).red());
        QCOMPARE(QGuiApplication::applicationDisplayName(), u"qKVM"_s);
    }

    void aWallWhereNothingChangesIsNotDrawn()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        // Too narrow for the bar's clock, which is the one thing on a wall that changes by itself.
        window->resize(1280, 800);
        QVERIFY(bringUp(window));
        QTest::qWait(600);
        QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
        // (Long enough for a clock to have gone on twice.)
        QTest::qWait(2600);
        QCOMPARE(drawn.count(), 0);

        // With room for the clock, the window is drawn when the clock goes on, and that is all.
        window->resize(1500, 800);
        QTRY_VERIFY(says(window->contentItem(), wall.clock()));
        QTest::qWait(600);
        drawn.clear();
        QTest::qWait(2600);
        QVERIFY2(drawn.count() >= 2 && drawn.count() <= 4, qPrintable(QString::number(drawn.count())));

        // A wall that is only shown has no bar and so no clock: once its own small bar has gone
        // and the screen has come to rest, nothing is drawn for as long as nothing changes. (In a
        // window wide enough for the clock: a clock that nobody sees is not a change.)
        wall.setDisplaying(true);
        const auto* small = window->findChild<QQuickItem*>(u"displayBar"_s);
        QVERIFY(small != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(small->opacity(), 0.0, 6000);
        int drawnAtRest = -1;
        for (int tries = 0; tries < 6 && drawnAtRest != 0; ++tries) {
            drawn.clear();
            QTest::qWait(2600);
            drawnAtRest = static_cast<int>(drawn.count());
        }
        QCOMPARE(drawnAtRest, 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whoeverHasJustSignedInHasTheKeyboardAtTheWall()
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

        // The password is typed, and Enter: the keyboard is in its line, as it has to be.
        type(window, u"a password"_s);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(signInAsked.count(), 1);
        QVERIFY(window->activeFocusItem() != nullptr);
        QVERIFY(window->activeFocusItem()->inherits("QQuickTextInput"));

        // The QKVM takes it, and the wall is there.
        session.show(signedIn());
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        const auto* wallItem = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(wallItem != nullptr);
        QTRY_VERIFY(wallItem->isVisible());
        // The keyboard is the wall's from that moment, without a click: not in a line nobody sees
        // any more, where a letter would be typed and a mark would go on blinking.
        QTRY_VERIFY(wallItem->hasActiveFocus());
        QVERIFY(!window->activeFocusItem()->inherits("QQuickTextInput"));
        // So the wall's own keys are its own at once: D shows the wall and nothing else, and "/"
        // goes to "Find a KVM".
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(wall.displaying());
        QTest::keyClick(window, Qt::Key_D);
        QTRY_VERIFY(!wall.displaying());
        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_VERIFY(theOne(window, u"findAKvm"_s) != nullptr && theOne(window, u"findAKvm"_s)->hasActiveFocus());

        // Signed out: the keyboard is where the next thing is typed, in the password's line again.
        shown.stage = u"signIn"_s;
        session.show(shown);
        QTRY_VERIFY(!wallItem->isVisible());
        QTRY_VERIFY(window->activeFocusItem() != nullptr && window->activeFocusItem()->inherits("QQuickTextInput"));
        QVERIFY(window->activeFocusItem()->isVisible());
        // And in again: the wall's once more.
        session.show(signedIn());
        QTRY_VERIFY(wallItem->hasActiveFocus());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aBoxOfTheWindowsOwnIsSaidToTheWallWhileItLiesOverIt()
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
        // (A wall that is only shown lets its groups wait meanwhile.)
        for (const QString& name : {u"aboutBox"_s, u"settingsBox"_s, u"exportBox"_s, u"importBox"_s}) {
            QObject* box = window->findChild<QObject*>(name);
            QVERIFY2(box != nullptr, qPrintable(name));
            QVERIFY2(!wall.covered(), qPrintable(name));
            QVERIFY(QMetaObject::invokeMethod(box, "open"));
            QTRY_VERIFY2(wall.covered(), qPrintable(name));
            QVERIFY(QMetaObject::invokeMethod(box, "close"));
            QTRY_VERIFY2(!wall.covered(), qPrintable(name));
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatLiesUnderABoxOrAMenuTakesNoClick()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const auto name = [&](int row) {
            return wall.machines()->index(row, 0).data(qkvm::ui::WallModel::NameRole).toString();
        };
        // In the picture of the wall's first card: at the left, where no box in the middle reaches.
        const QQuickItem* first = saying(window->contentItem(), name(0));
        QVERIFY(first != nullptr);
        const QPoint onTheCard = first->mapToScene(QPointF(4, -40)).toPoint();
        // (A click there enlarges the card, where nothing lies over it.)
        clickOn(window, onTheCard);
        QTRY_VERIFY(!wall.enlarged().isEmpty());
        wall.closeEnlarged();
        QTRY_VERIFY(window->findChild<QQuickItem*>(u"wall"_s)->isVisible());

        // A box lies over the wall. A click beside it closes it, and that is all it does: the
        // card under the click has not been clicked. (These were double clicks while a double
        // click opened a card; one click does now.)
        QObject* box = window->findChild<QObject*>(u"aboutBox"_s);
        QVERIFY(QMetaObject::invokeMethod(box, "open"));
        QTRY_VERIFY(box->property("opened").toBool());
        clickOn(window, onTheCard);
        QTRY_VERIFY(!box->property("opened").toBool());
        QTest::qWait(100);
        QCOMPARE(wall.enlarged(), QString());

        // A menu of the bar hangs over the cards, and so does the list of a choice in it. A click
        // on the choice opens its list, and one on a row of the list chooses the row; the card
        // under either is not enlarged by it.
        clickOn(window, theOne(window, u"viewButton"_s));
        const QObject* menu = window->findChild<QObject*>(u"viewMenu"_s);
        QTRY_VERIFY(menu->property("opened").toBool());
        const QQuickItem* order = theOneSeen(window, u"orderChoice"_s);
        QVERIFY(order != nullptr);
        const QPoint onTheChoice = order->mapToScene(QPointF(order->width() / 2, order->height() / 2)).toPoint();
        QVERIFY(cardAt(window->contentItem(), onTheChoice) != nullptr);
        QTest::mouseClick(window, Qt::LeftButton, {}, onTheChoice);
        QTRY_VERIFY(seenNamed(window, u"choice"_s) > 0);
        QList<const QQuickItem*> entries;
        named(window->contentItem(), u"choice"_s, entries);
        const auto entry = std::ranges::find_if(
            entries, [](const QQuickItem* one) { return isSeen(one) && saying(one, u"Problems first"_s) != nullptr; });
        QVERIFY(entry != entries.end());
        const QPoint onTheRow = (*entry)->mapToScene(QPointF((*entry)->width() / 2, (*entry)->height() / 2)).toPoint();
        QVERIFY(cardAt(window->contentItem(), onTheRow) != nullptr);
        QTest::mouseClick(window, Qt::LeftButton, {}, onTheRow);
        QTRY_COMPARE(wall.shown()->order(), u"state"_s);
        QTRY_COMPARE(seenNamed(window, u"choice"_s), 0);
        QTest::qWait(100);
        QCOMPARE(wall.enlarged(), QString());
        QVERIFY(menu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTest::qWait(100);
        QCOMPARE(wall.enlarged(), QString());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // What lies over the wall takes the click that lands on it, as on the page: the bar and the
    // banner over cards that are scrolled up under them, a notice in the corner, display mode's
    // small bar, and the bars at the foot while machines are picked or the wall's order is made.
    // No card under them is shown large by it, picked or dragged.
    void whatLiesOverTheWallTakesTheClicksThatLandOnIt()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown = signedIn();
        session.show(shown);
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        auto* scrolled = window->findChild<QQuickItem*>(u"wall"_s);
        QVERIFY(scrolled != nullptr);
        // The widest stretch of a bar that no control stands in.
        const auto bare = [](const QQuickItem* strip) {
            QList<QRectF> taken;
            const std::function<void(const QQuickItem*)> walk = [&](const QQuickItem* under) {
                if (!under->isVisible()) {
                    return;
                }
                if (under->inherits("QQuickAbstractButton") || under->inherits("QQuickTextInput")) {
                    taken.append(under->mapRectToScene(QRectF(0, 0, under->width(), under->height())));
                    return;
                }
                const QList<QQuickItem*> children = under->childItems();
                for (const QQuickItem* child : children) {
                    walk(child);
                }
            };
            walk(strip);
            const QRectF whole = strip->mapRectToScene(QRectF(0, 0, strip->width(), strip->height()));
            std::ranges::sort(taken, {}, &QRectF::left);
            qreal from = whole.left();
            QRectF widest;
            for (const QRectF& control : std::as_const(taken)) {
                if (control.left() - from > widest.width()) {
                    widest = QRectF(from, whole.top(), control.left() - from, whole.height());
                }
                from = std::max(from, control.right());
            }
            if (whole.right() - from > widest.width()) {
                widest = QRectF(from, whole.top(), whole.right() - from, whole.height());
            }
            return widest;
        };
        // A place in the middle of a part of the window where a card lies under it, and not the gap
        // between two; none where no card does.
        const auto overACard = [&](const QRectF& part) {
            QRectF widest;
            const QList<const QQuickItem*> cards = [&] {
                QList<const QQuickItem*> all;
                for (const QString& key : made(wall)) {
                    all.append(cardOf(window->contentItem(), key));
                }
                return all;
            }();
            for (const QQuickItem* card : cards) {
                const QRectF both = card->mapRectToScene(QRectF(0, 0, card->width(), card->height())).intersected(part);
                if (both.width() > widest.width() && both.height() >= part.height() / 2) {
                    widest = both;
                }
            }
            return widest.isEmpty() ? QPoint() : QPointF(widest.center().x(), part.center().y()).toPoint();
        };
        const auto whole = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const auto nothingIsShownLarge = [&](const QPoint& at) {
            clickOn(window, at);
            QTest::qWait(100);
            return wall.enlarged().isEmpty();
        };

        // The bar, over the first row of cards, which is scrolled up under it.
        QVERIFY(scrolled->property("contentHeight").toReal() > scrolled->height() + 100);
        QVERIFY(scrolled->setProperty("contentY", 100));
        const QQuickItem* bar = window->findChild<QQuickItem*>(u"topBar"_s);
        QVERIFY(bar != nullptr);
        letTheWallComeToRest(window);
        const QPoint onTheBar = overACard(bare(bar));
        QVERIFY(!onTheBar.isNull());
        QVERIFY(nothingIsShownLarge(onTheBar));

        // The banner of a server that does not answer, over that row too.
        shown.inContact = false;
        shown.awaySince = u"14:02"_s;
        session.show(shown);
        const QQuickItem* banner = window->findChild<QQuickItem*>(u"banner"_s);
        QVERIFY(banner != nullptr);
        QTRY_VERIFY(banner->isVisible() && banner->height() > 0);
        letTheWallComeToRest(window);
        const QPoint onTheBanner = overACard(whole(banner));
        QVERIFY(!onTheBanner.isNull());
        QVERIFY(nothingIsShownLarge(onTheBanner));
        shown.inContact = true;
        session.show(shown);
        QTRY_VERIFY(!banner->isVisible());

        // A notice in the corner.
        QVERIFY(scrolled->setProperty("contentY", 0));
        actions.notices()->tell(qkvm::ui::Notices::Kind::Done, u"NAS: power button pressed"_s);
        QTRY_VERIFY(saying(window->contentItem(), u"NAS: power button pressed"_s) != nullptr);
        letTheWallComeToRest(window);
        const QQuickItem* told = theOneSeen(window, u"notice"_s);
        QVERIFY(told != nullptr);
        const QPoint onTheNotice = overACard(whole(told));
        QVERIFY(!onTheNotice.isNull());
        QVERIFY(nothingIsShownLarge(onTheNotice));
        actions.notices()->clear();

        // The bar at the foot while machines are picked: no card under it is picked by it.
        wall.setSelecting(true);
        const QQuickItem* bulk = theOne(window, u"bulkBar"_s);
        QVERIFY(bulk != nullptr);
        QTRY_VERIFY(bulk->isVisible());
        letTheWallComeToRest(window);
        const QPoint onTheBulkBar = overACard(bare(bulk));
        QVERIFY(!onTheBulkBar.isNull());
        clickOn(window, onTheBulkBar);
        QTest::qWait(100);
        QCOMPARE(wall.picked(), 0);
        wall.setSelecting(false);

        // The bar at the foot while the wall's order is made, and the bar at the top, which rests
        // meanwhile: no card under them is dragged from there.
        wall.arrange();
        QTRY_VERIFY(wall.arranging());
        const QStringList before = made(wall);
        const QQuickItem* foot = theOne(window, u"arrangeBar"_s);
        QVERIFY(foot != nullptr);
        QTRY_VERIFY(foot->isVisible());
        QVERIFY(scrolled->setProperty("contentY", 100));
        letTheWallComeToRest(window);
        const auto noCardIsDraggedFrom = [&](const QPoint& from) {
            const QQuickItem* another = cardOf(window->contentItem(), before.at(6));
            const QPoint to = another->mapToScene(QPointF(another->width() * 0.25, another->height() / 2)).toPoint();
            QTest::mousePress(window, Qt::LeftButton, {}, from);
            QTest::mouseMove(window, from + QPoint(40, 10));
            QTest::mouseMove(window, to + QPoint(5, 5));
            QTest::mouseMove(window, to);
            QTest::qWait(100);
            const bool dragged = theOne(window, u"draggedCard"_s)->isVisible();
            QTest::mouseRelease(window, Qt::LeftButton, {}, to);
            QTest::qWait(100);
            return !dragged && made(wall) == before;
        };
        const QPoint onTheFoot = overACard(bare(foot));
        QVERIFY(!onTheFoot.isNull());
        QVERIFY(noCardIsDraggedFrom(onTheFoot));
        const QPoint onTheRestingBar = overACard(bare(bar));
        QVERIFY(!onTheRestingBar.isNull());
        QVERIFY(noCardIsDraggedFrom(onTheRestingBar));
        wall.stopArranging();
        QTRY_VERIFY(!wall.arranging());

        // Display mode's small bar, where it has no button.
        QVERIFY(scrolled->setProperty("contentY", 0));
        wall.setDisplaying(true);
        auto* small = window->findChild<QQuickItem*>(u"displayBar"_s);
        QVERIFY(small != nullptr);
        QTRY_VERIFY(small->isVisible());
        QVERIFY(QMetaObject::invokeMethod(small, "wake"));
        QTRY_COMPARE(small->opacity(), 1.0);
        letTheWallComeToRest(window);
        const QPoint onTheSmallBar = overACard(bare(small));
        QVERIFY(!onTheSmallBar.isNull());
        QVERIFY(nothingIsShownLarge(onTheSmallBar));
        QVERIFY(wall.displaying());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aWindowThatIsMinimizedOrOutOfSightSaysSoToTheWall()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        // Not shown yet: nobody sees it.
        QVERIFY(wall.isUnseen());
        QVERIFY(bringUp(window));
        QTRY_VERIFY(!wall.isUnseen());

        window->showMinimized();
        QTRY_VERIFY(wall.isUnseen());
        window->showNormal();
        QTRY_VERIFY(!wall.isUnseen());
        window->showMaximized();
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        QVERIFY(!wall.isUnseen());
        window->showFullScreen();
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        QVERIFY(!wall.isUnseen());
        // Behind the program's icon.
        window->hide();
        QTRY_VERIFY(wall.isUnseen());
        window->setVisible(true);
        QTRY_VERIFY(!wall.isUnseen());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aServerThatDoesNotAnswerIsSaidOverTheWall()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        qkvm::ui::SessionView::Shown shown = signedIn();
        session.show(shown);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto* banner = window->findChild<QQuickItem*>(u"banner"_s);
        QVERIFY(banner != nullptr);
        QVERIFY(!banner->isVisible());
        // No card's screen is dimmed.
        QCOMPARE(seenNamed(window, u"oldVeil"_s), 0);

        // And where what goes to the server is not encrypted, the bar says so for as long as it is so.
        const auto* notEncrypted = window->findChild<QQuickItem*>(u"barNotEncrypted"_s);
        QVERIFY(notEncrypted != nullptr);
        QVERIFY(notEncrypted->isVisible());
        shown.encrypted = true;
        session.show(shown);
        QTRY_VERIFY(!notEncrypted->isVisible());
        shown.encrypted = false;
        session.show(shown);
        QTRY_VERIFY(notEncrypted->isVisible());

        shown.inContact = false;
        shown.awaySince = u"10/10/2026 14:02"_s;
        session.show(shown);
        QTRY_VERIFY(banner->isVisible());
        QVERIFY(says(banner, u"Lost contact with qKVM at 10/10/2026 14:02. Trying again…"_s));
        // The wall is still there under it, with what it last knew: and every card's screen is
        // dimmed, so that no picture of a while ago is taken for the machine as it is.
        QCOMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        QCOMPARE(seenNamed(window, u"oldVeil"_s), 15);
        keepPictureOf(window, u"wall-lost-contact"_s);

        // So is a machine that is shown large: the one screen that is seen then.
        wall.enlarge(qkvm::ui::pretendWall()[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedScreen"_s) != nullptr);
        QCOMPARE(seenNamed(window, u"oldVeil"_s), 1);
        QVERIFY(banner->isVisible());
        keepPictureOf(window, u"enlarged-lost-contact"_s);

        shown.inContact = true;
        shown.awaySince.clear();
        session.show(shown);
        QTRY_VERIFY(!banner->isVisible());
        QCOMPARE(seenNamed(window, u"oldVeil"_s), 0);
        wall.closeEnlarged();
        QTRY_VERIFY(theOne(window, u"enlargedScreen"_s) == nullptr);
        QCOMPARE(seenNamed(window, u"oldVeil"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runScreensTests<TestWallScreens>(argc, argv);
}

#include "test_wall_screens.moc"
