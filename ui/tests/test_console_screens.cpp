#include "screens_test.h"

// A machine shown large: its bar, its picture, its keyboard and mouse and what is asked before
// keys go, its details, and how it fits the window.
class TestConsoleScreens : public ScreensTest {
    Q_OBJECT

private slots:
    // After each test, when its windows are gone: what they were handed goes too.
    void cleanup() { forgetWhatTheWindowsWereHanded(); }

    void workspacesAndSourceSelectionFitSmallAndLargeWindows_data()
    {
        screenshotAndRelativeControlsFitTheEnlargedScreen_data();
    }

    void workspacesAndSourceSelectionFitSmallAndLargeWindows()
    {
        QFETCH(QSize, size);
        QFETCH(QString, theme);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(atItsOwnWall());
        session.showSources({QVariantMap{{u"value"_s, u"pc"_s}, {u"label"_s, u"This PC"_s}},
                             QVariantMap{{u"value"_s, u"https://example.test/"_s}, {u"label"_s, u"example.test"_s}}},
                            u"pc"_s, true);
        QTemporaryDir folder;
        qkvm::core::Settings settings(folder.filePath(u"settings.json"_s));
        qkvm::ui::WorkspacesView workspaces(settings);
        int restored = 0;
        workspaces.useWindows([] { return QList<qkvm::core::WallWindow>{qkvm::core::WallWindow{}}; },
                              [&](const auto&) {
                                  ++restored;
                                  return true;
                              });
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(window->setProperty("workspaces", QVariant::fromValue(&workspaces)));
        window->resize(size);
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        QVERIFY(bringUp(window));
        QVERIFY(window->title().contains(u"This PC"_s));
        // The source of this window, all the sources and the saved workspaces are in the account's
        // drop: whole inside the window, with the keys that do the same.
        auto* menu = window->findChild<QObject*>(u"accountMenu"_s);
        const QQuickItem* account = theOne(window, u"accountButton"_s);
        QVERIFY(menu != nullptr && account != nullptr);
        clickOn(window, account);
        QTRY_VERIFY(menu->property("opened").toBool());
        auto* inside = menu->property("flick").value<QQuickItem*>();
        QTRY_VERIFY(isInside(window, inside));
        QCOMPARE(chosenIn(window, u"sourceChoice"_s), u"This PC"_s);
        for (const QString& line : {u"Source for this window"_s, u"Browse all sources…"_s, u"Ctrl+Shift+S"_s,
                                    u"Saved workspaces…"_s, u"Ctrl+Shift+W"_s, u"Connect to a qKVM server"_s}) {
            const QQuickItem* said = rowOf(menu, line);
            QVERIFY2(said != nullptr, qPrintable(line));
            QVERIFY2(isInside(window, said), qPrintable(line));
            QVERIFY2(said->width() >= said->implicitWidth(), qPrintable(line));
        }
        keepPictureOf(window, u"account-sources-%1-%2"_s.arg(size.width()).arg(theme));
        // In a window too low for all of it, it scrolls; what it scrolls out over its top is not
        // drawn over the bar: a row or a choice half out, or just out.
        QVERIFY(menu->setProperty("room", 150));
        QTRY_VERIFY(inside->property("scrolls").toBool());
        const QImage above = stripAbove(window, inside);
        QVERIFY(!above.isNull());
        const auto* content = inside->property("contentItem").value<QQuickItem*>();
        for (const QString& name : {u"sourceChoice"_s, u"browseSources"_s}) {
            const QQuickItem* one = theOneSeen(window, name);
            QVERIFY2(one != nullptr, qPrintable(name));
            const qreal at = one->mapToItem(content, QPointF(0, 0)).y();
            // (A row reaches 4 px past its place above and below, as the page's does.)
            const qreal reach = name == u"browseSources"_s ? 4 : 0;
            QVERIFY(inside->setProperty("contentY", at + one->height() / 2));
            QVERIFY2(stripAbove(window, inside) == above, qPrintable(name));
            QVERIFY(inside->setProperty("contentY", at + one->height() + reach + 2));
            QVERIFY2(stripAbove(window, inside) == above, qPrintable(name));
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        // Another source is chosen there, and the drop closes.
        QSignalSpy selected(&session, &qkvm::ui::SessionView::sourceAsked);
        clickOn(window, account);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(chooseIn(window, u"sourceChoice"_s, u"example.test"_s));
        QTRY_COMPARE(selected.size(), 1);
        QCOMPARE(selected.first().first().toString(), u"https://example.test/"_s);
        QTRY_VERIFY(!menu->property("opened").toBool());
        session.showSourceTree({QVariantMap{{u"key"_s, u"pc"_s},
                                            {u"depth"_s, 0},
                                            {u"source"_s, u"pc"_s},
                                            {u"label"_s, u"This PC"_s},
                                            {u"status"_s, u"Connected"_s}},
                                QVariantMap{{u"key"_s, u"pc/g/Office"_s},
                                            {u"depth"_s, 1},
                                            {u"parentKey"_s, u"pc"_s},
                                            {u"label"_s, u"Office"_s},
                                            {u"status"_s, u"1 machine"_s}},
                                QVariantMap{{u"key"_s, u"pc/m/a"_s},
                                            {u"depth"_s, 2},
                                            {u"parentKey"_s, u"pc/g/Office"_s},
                                            {u"ancestorKey"_s, u"pc"_s},
                                            {u"scope"_s, u"pc"_s},
                                            {u"machine"_s, u"a"_s},
                                            {u"label"_s, u"Build machine"_s},
                                            {u"status"_s, u"Online"_s}},
                                QVariantMap{{u"key"_s, u"server"_s},
                                            {u"depth"_s, 0},
                                            {u"source"_s, u"https://example.test/"_s},
                                            {u"label"_s, u"example.test"_s},
                                            {u"status"_s, u"Open to reconnect"_s}}});
        auto* sources = window->findChild<QObject*>(u"sourcesBox"_s);
        QVERIFY(sources != nullptr);
        // As Ctrl+Shift+S does, as before.
        QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(sources->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!sources->property("opened").toBool());
        clickOn(window, account);
        QTRY_VERIFY(menu->property("opened").toBool());
        clickOn(window, theOneSeen(window, u"browseSources"_s));
        QTRY_VERIFY(sources->property("opened").toBool());
        QTRY_VERIFY(!menu->property("opened").toBool());
        auto* search = window->findChild<QQuickItem*>(u"sourceSearch"_s);
        auto* tree = window->findChild<QQuickItem*>(u"sourceTree"_s);
        QVERIFY(search != nullptr && tree != nullptr);
        QVERIFY(isInside(window, search));
        QVERIFY(isInside(window, tree));
        QTRY_COMPARE(tree->property("count").toInt(), 4);
        const auto* expand = theOne(window, u"sourceExpand-pc"_s);
        QVERIFY(expand != nullptr);
        clickOn(window, expand);
        QTRY_COMPARE(tree->property("count").toInt(), 2);
        QVERIFY(search->setProperty("text", u"Build"_s));
        QTRY_COMPARE(tree->property("count").toInt(), 3);
        keepPictureOf(window, u"sources-%1-%2"_s.arg(size.width()).arg(theme));
        QVERIFY(QMetaObject::invokeMethod(tree, "positionViewAtEnd"));
        QSignalSpy opened(&session, &qkvm::ui::SessionView::machineAsked);
        const auto* machine = theOne(window, u"sourceRow-pc/m/a"_s);
        QVERIFY(machine != nullptr);
        QTRY_VERIFY(isInside(window, machine));
        clickOn(window, machine);
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened.first(), (QList<QVariant>{u"pc"_s, u"a"_s}));
        QTRY_VERIFY(!sources->property("opened").toBool());
        auto* box = window->findChild<QObject*>(u"workspacesBox"_s);
        QVERIFY(box != nullptr);
        // As Ctrl+Shift+W does, as before.
        QTest::keyClick(window, Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(box->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!box->property("opened").toBool());
        clickOn(window, account);
        QTRY_VERIFY(menu->property("opened").toBool());
        clickOn(window, theOneSeen(window, u"savedWorkspaces"_s));
        QTRY_VERIFY(box->property("opened").toBool());
        QTRY_VERIFY(!menu->property("opened").toBool());
        auto* name = window->findChild<QQuickItem*>(u"workspaceName"_s);
        QVERIFY(name != nullptr);
        QVERIFY(name->setProperty("text", u"Office monitors"_s));
        const auto* save = theOne(window, u"workspaceSave"_s);
        QVERIFY(save != nullptr);
        QVERIFY(isInside(window, save));
        clickOn(window, save);
        QTRY_COMPARE(workspaces.entries().size(), 1);
        QVERIFY(box->setProperty("selected", settings.remembered().workspaces.first().id));
        keepPictureOf(window, u"workspaces-%1-%2"_s.arg(size.width()).arg(theme));
        auto* scroll = window->findChild<QQuickItem*>(u"workspaceScroll"_s);
        QVERIFY(scroll != nullptr);
        QVERIFY(scroll->setProperty("contentY",
                                    std::max(0.0, scroll->property("contentHeight").toDouble() - scroll->height())));
        const auto* open = theOne(window, u"workspaceOpen"_s);
        QVERIFY(open != nullptr);
        QTRY_VERIFY(isInside(window, open));
        clickOn(window, open);
        QTRY_COMPARE(restored, 1);
        QTRY_VERIFY(!box->property("opened").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void screenshotAndRelativeControlsFitTheEnlargedScreen_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("theme");
        QTest::newRow("small-dark") << QSize(640, 400) << u"dark"_s;
        QTest::newRow("small-light") << QSize(640, 400) << u"light"_s;
        QTest::newRow("large-dark") << QSize(1280, 800) << u"dark"_s;
        QTest::newRow("large-light") << QSize(1280, 800) << u"light"_s;
    }

    void screenshotAndRelativeControlsFitTheEnlargedScreen()
    {
        QFETCH(QSize, size);
        QFETCH(QString, theme);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        auto tiles = qkvm::ui::pretendWall();
        tiles[0].actable = true;
        wall.machines()->setTiles(tiles);
        wall.setFavorites({tiles[0].key}, true);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        int captures = 0;
        int copied = 0;
        int mouseTakes = 0;
        int mouseReleases = 0;
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(size);
        m_looks.back()->choose(theme);
        // Offscreen Qt does not apply Windows' color scheme; set the same choice
        // on the theme singleton, as the other rendered screen tests do.
        QObject* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        QVERIFY(bringUp(window));
        QTest::mouseMove(window, QPoint(size.width() - 4, 4));
        QTRY_VERIFY(theOneSeen(window, u"cardFavorite"_s) != nullptr);
        auto* star = theOneSeen(window, u"cardFavorite"_s);
        QTRY_VERIFY(star->isVisible());
        const QSignalSpy favorite(&wall, &qkvm::ui::WallView::favoriteAsked);
        clickOn(window, star);
        QCOMPARE(favorite.size(), 1);
        QCOMPARE(favorite.front().front().toString(), tiles[0].key);
        keepPictureOf(window, u"favorite-wall-%1-%2"_s.arg(size.width()).arg(theme));
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOne(window, u"screenCapture"_s) != nullptr);
        auto* capture = window->findChild<qkvm::ui::ScreenCapture*>(u"screenCapture"_s);
        QVERIFY(capture != nullptr);
        capture->captureWith([&](quint32, const qkvm::ui::ScreenCapture::Done& done) {
            ++captures;
            QImage image(8, 8, QImage::Format_ARGB32);
            image.fill(Qt::red);
            done(image);
        });
        capture->copyWith([&](const QImage&) {
            ++copied;
            return true;
        });
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.picture = QSize(1920, 1080);
        m_control.show(shown);
        for (const QString& name :
             {u"saveScreenshot"_s, u"copyScreenshot"_s, u"enlargedFavorite"_s, u"relativeMouse"_s, u"alwaysOnTop"_s}) {
            QVERIFY(revealConsoleItem(window, name));
            const QQuickItem* button = theOne(window, name);
            QVERIFY(button != nullptr);
            QTRY_VERIFY(button->isVisible() && button->isEnabled());
            const QRectF area = button->mapRectToScene(QRectF(0, 0, button->width(), button->height()));
            QVERIFY2(QRectF(QPointF(), size).contains(area), qPrintable(name));
            for (const QString& menu : {u"consoleMore"_s, u"inputMore"_s}) {
                auto* popup = window->findChild<QObject*>(menu);
                if (popup != nullptr && popup->property("visible").toBool())
                    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
            }
        }
        QCOMPARE(captures, 0);
        QVERIFY(revealConsoleItem(window, u"copyScreenshot"_s));
        clickOn(window, theOne(window, u"copyScreenshot"_s));
        QCOMPARE(captures, 1);
        QCOMPARE(copied, 1);
        QVERIFY(revealConsoleItem(window, u"enlargedFavorite"_s));
        clickOn(window, theOne(window, u"enlargedFavorite"_s));
        QCOMPARE(favorite.size(), 2);
        QCOMPARE(favorite.front().front().toString(), tiles[0].key);
        const QSignalSpy relative(&m_control, &qkvm::ui::ControlView::relativeAsked);
        QVERIFY(revealConsoleItem(window, u"relativeMouse"_s));
        clickOn(window, theOne(window, u"relativeMouse"_s));
        QCOMPARE(relative.count(), 1);
        QCOMPARE(relative.first().first().toBool(), true);
        shown.relative = true;
        m_control.show(shown);
        QTest::qWait(80);
        keepPictureOf(window, u"console-tools-%1-%2"_s.arg(size.width()).arg(theme));
        auto* pointer = window->findChild<qkvm::ui::RelativePointer*>(u"relativePointer"_s);
        QVERIFY(pointer != nullptr);
        pointer->useSystem({[&](quintptr) {
                                ++mouseTakes;
                                return true;
                            },
                            [&] { ++mouseReleases; }, [](const QRect&) { return true; }, [] {},
                            [](quintptr) { return true; }, [](std::uint16_t) { return false; },
                            [](quintptr) { return std::optional<QPoint>(); }},
                           [](QQuickWindow*, QRectF rectangle) { return rectangle.toAlignedRect(); });
        connect(&m_control, &qkvm::ui::ControlView::captureBegan, &engine, [&] {
            shown.pointerCaptured = true;
            m_control.show(shown);
        });
        connect(&m_control, &qkvm::ui::ControlView::captureEnded, &engine, [&] {
            shown.on = false;
            shown.pointerCaptured = false;
            m_control.show(shown);
        });
        shown.on = true;
        m_control.show(shown);
        QTRY_VERIFY(m_control.pointerCaptured());
        QTest::qWait(180);
        QVERIFY(m_control.pointerCaptured());
        QCOMPARE(mouseTakes, 1);
        QVERIFY(!capture->possible());
        keepPictureOf(window, u"relative-control-%1-%2"_s.arg(size.width()).arg(theme));
        // Moving/resizing the picture gives input back; it never captures again by itself.
        window->resize(size + QSize(80, 60));
        QTRY_VERIFY(!m_control.isOn());
        QCOMPARE(mouseReleases, 1);
        QTest::qWait(120);
        QCOMPARE(mouseTakes, 1);
        actions.setAllowed(false);
        QVERIFY(!capture->possible());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void consolePinIsExplicitAndClearsWhenLeavingTheConsole()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        QVERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOne(window, u"alwaysOnTop"_s) != nullptr);
        clickOn(window, theOne(window, u"alwaysOnTop"_s));
        QTRY_VERIFY(window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(window->isVisible());
        wall.closeEnlarged();
        QTRY_VERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(!window->property("consoleOnTop").toBool());
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOne(window, u"alwaysOnTop"_s) != nullptr);
        QVERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        clickOn(window, theOne(window, u"alwaysOnTop"_s));
        QTRY_VERIFY(window->flags().testFlag(Qt::WindowStaysOnTopHint));
        window->showFullScreen();
        QTRY_VERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(!theOne(window, u"alwaysOnTop"_s)->isEnabled());
        window->showNormal();
        QTRY_VERIFY(theOne(window, u"alwaysOnTop"_s)->isEnabled());
        QVERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        clickOn(window, theOne(window, u"alwaysOnTop"_s));
        QTRY_VERIFY(window->flags().testFlag(Qt::WindowStaysOnTopHint));
        session.show({});
        QTRY_VERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void changingTheReleaseSettingUpdatesTheConsoleHint()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        wall.enlarge(tiles[0].key);
        m_ownSettings.back()->setReleaseKey(u"scroll-lock"_s);
        QTRY_COMPARE(m_control.leaveKeyName(), u"Scroll Lock"_s);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.relative = true;
        m_control.show(shown);
        QTRY_VERIFY(theOne(window, u"relativeMouse"_s)->property("hint").toString().contains(u"Scroll Lock"_s));
        m_ownSettings.back()->setReleaseKey(u"f12"_s);
        QTRY_VERIFY(theOne(window, u"relativeMouse"_s)->property("hint").toString().contains(u"F12"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // What the live picture's own numbers say, in its details and in the bar under it: how many
    // pictures reach the screen and how soon, which are not shown, the long gaps, and on which
    // graphics card. The bar says what reaches the screen, where that is known.
    void theDetailsSayWhatReachesTheScreen()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        wall.machines()->setTiles(qkvm::ui::pretendWall());
        qkvm::ui::SessionView session;
        session.show(signedIn());
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall);
        QVERIFY(window != nullptr);
        window->resize(1280, 800);
        QVERIFY(bringUp(window));
        wall.enlarge(qkvm::ui::pretendWall()[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedScreen"_s) != nullptr);
        wall.setLiveStream(4711);
        wall.setLive(true);
        wall.setGraphicsCard(u"A graphics card"_s);
        wall.setLiveDiagnostics({{u"connected"_s, true},      {u"transport"_s, u"webrtc"_s},
                                 {u"hasPicture"_s, true},     {u"codec"_s, u"h264"_s},
                                 {u"width"_s, 1920},          {u"height"_s, 1080},
                                 {u"rateKnown"_s, true},      {u"receivedFps"_s, 60.2},
                                 {u"receivedMbps"_s, 7.5},    {u"lastReceivedMs"_s, 9},
                                 {u"decoding"_s, u"gpu"_s},   {u"engineKnown"_s, true},
                                 {u"decoded"_s, 1800},        {u"decodeMs"_s, 0.8},
                                 {u"shown"_s, 1799},          {u"shownFps"_s, 59.9},
                                 {u"shownRateKnown"_s, true}, {u"arrivedToShownMs"_s, 2.74},
                                 {u"replaced"_s, 1},          {u"dropped"_s, 2},
                                 {u"refused"_s, 3},           {u"lateSteps"_s, 4},
                                 {u"sourceLateSteps"_s, 5}});
        QTRY_VERIFY(theOne(window, u"enlargedLive"_s)->property("text").toString().contains(u" · 59.9/s"_s));
        clickOn(window, theOne(window, u"enlargedVideoDetails"_s));
        QTRY_VERIFY(theOne(window, u"videoDiagnosticsText"_s) != nullptr);
        const QString said = theOne(window, u"videoDiagnosticsText"_s)->property("text").toString();
        for (const QString& line : {u"Received: 60.2 frames/s · 7.50 Mbit/s payload"_s,
                                    u"Shown: 59.9 frames/s · 2.7 ms from arrival to screen"_s,
                                    u"Decoder: Graphics card"_s, u"Drawn on: A graphics card"_s,
                                    u"Not shown: 1 made way for a newer one · 2 dropped behind · 3 rejected"_s,
                                    u"Gaps over 38 ms: 4 on screen · 5 as they arrived"_s}) {
            QVERIFY2(said.split(u'\n').contains(line), qPrintable(said));
        }
        keepPictureOf(window, u"console-video-details"_s);
        // Before the rate on the screen is known, the bar says what arrives, and the details say no
        // rate on the screen.
        QVariantMap before = wall.liveDiagnostics();
        before.insert(u"shownRateKnown"_s, false);
        before.insert(u"shownFps"_s, 0.0);
        wall.setLiveDiagnostics(before);
        QTRY_VERIFY(theOne(window, u"enlargedLive"_s)->property("text").toString().contains(u" · 60.2/s"_s));
        QVERIFY(!theOne(window, u"videoDiagnosticsText"_s)->property("text").toString().contains(u"Shown:"_s));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void aMachineThatIsHadLiveIsDrawnFromItsLivePictures()
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
        wall.enlarge(qkvm::ui::pretendWall()[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedScreen"_s) != nullptr);
        const QQuickItem* screen = theOne(window, u"enlargedScreen"_s);
        // Its snapshots, while it is not had live: the stream its card names.
        const int own = screen->property("stream").toInt();
        QVERIFY(own != 0);
        QCOMPARE(seenNamed(window, u"enlargedLive"_s), 1);
        // Had live: its live pictures, and the bar says so.
        wall.setLiveStream(4711);
        wall.setLive(true);
        QTRY_COMPARE(screen->property("stream").toInt(), 4711);
        QTRY_COMPARE(seenNamed(window, u"enlargedLive"_s), 1);
        QVERIFY(theOne(window, u"enlargedLive"_s)->property("text").toString().startsWith(u"Live"_s));
        keepPictureOf(window, u"enlarged-live"_s);
        // The pictures have stopped, and the last one stays up: drawn, and not said to be live.
        wall.setLive(false);
        QTRY_COMPARE(seenNamed(window, u"enlargedLive"_s), 1);
        QCOMPARE(screen->property("stream").toInt(), 4711);
        // The line ended: its snapshots again.
        wall.setLiveStream(0);
        QTRY_COMPARE(screen->property("stream").toInt(), own);
        QCOMPARE(seenNamed(window, u"enlargedLive"_s), 1);

        // A server whose screen can be read, and of which no snapshot has come yet: it says how
        // it is doing where its screen will be. Its live screen is not hidden behind that.
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        const auto server =
            std::ranges::find_if(tiles, [](const qkvm::ui::WallTile& tile) { return !tile.server.isEmpty(); });
        QVERIFY(server != tiles.end());
        server->stream = 99;
        wall.machines()->setTiles(tiles);
        wall.enlarge(server->key);
        QTRY_COMPARE(seenNamed(window, u"serverPanel"_s), 1);
        wall.setLiveStream(4712);
        QTRY_COMPARE(seenNamed(window, u"serverPanel"_s), 0);
        QCOMPARE(theOne(window, u"enlargedScreen"_s)->property("stream").toInt(), 4712);
        wall.setLiveStream(0);
        QTRY_COMPARE(seenNamed(window, u"serverPanel"_s), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theEnlargedViewHasTheWayIntoTheMachinesMenu()
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
        const QSignalSpy openAsked(&actions, &qkvm::ui::ActionsView::openAsked);
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedActions"_s) != nullptr);
        const QQuickItem* button = theOne(window, u"enlargedActions"_s);
        // Not for whoever may only look.
        QVERIFY(!button->isVisible());
        actions.setAllowed(true);
        QTRY_VERIFY(button->isVisible());
        QVERIFY(says(button, u"Actions"_s));
        keepPictureOf(window, u"enlarged-actions"_s);
        clickOn(window, button);
        QTRY_COMPARE(openAsked.count(), 1);
        QCOMPARE(openAsked[0][0].toString(), tiles[1].key);
        // The keys of the enlarged view still work afterwards: Esc goes back to the wall.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void ownershipAndTakeoverConfirmationAreVisibleAndFitASmallWindow()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window);
        QVERIFY(bringUp(window));
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.ownerName = u"Operator <one>"_s;
        shown.takeoverPossible = true;
        m_control.show(shown);
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s));
        QTRY_COMPARE(seenNamed(window, u"controlOwner"_s), 1);
        QVERIFY(says(theOne(window, u"controlOwner"_s), u"Controlled by Operator <one> (last reported)"_s));
        QSignalSpy answers(&m_control, &qkvm::ui::ControlView::takeoverAnswered);
        connect(&m_control, &qkvm::ui::ControlView::takeoverAsked, &engine, [&] {
            shown.takeoverPending = true;
            shown.takeoverOwner = shown.ownerName;
            m_control.show(shown);
        });
        connect(&m_control, &qkvm::ui::ControlView::takeoverAnswered, &engine, [&](bool) {
            shown.takeoverPending = false;
            m_control.show(shown);
        });
        keepPictureOf(window, u"control-owner"_s);
        clickOn(window, theOne(window, u"enlargedControl"_s));
        QTRY_COMPARE(seenNamed(window, u"controlTakeoverNo"_s), 1);
        auto* cancel = theOne(window, u"controlTakeoverNo"_s);
        QTRY_VERIFY(cancel->hasActiveFocus());
        QVERIFY(theOne(window, u"controlTakeoverText"_s)->property("text").toString().contains(shown.ownerName));
        keepPictureOf(window, u"control-takeover"_s);
        clickOn(window, cancel);
        QTRY_COMPARE(answers.size(), 1);
        QVERIFY(!answers.last().first().toBool());
        QTRY_COMPARE(seenNamed(window, u"controlTakeoverNo"_s), 0);
        window->resize(640, 400);
        shown.ownerName = QString(1024, u'W');
        m_control.show(shown);
        m_control.take();
        QTRY_COMPARE(seenNamed(window, u"controlTakeoverYes"_s), 1);
        auto* confirm = theOne(window, u"controlTakeoverYes"_s);
        QTRY_VERIFY(isInside(window, confirm));
        QTRY_VERIFY(isInside(window, theOne(window, u"controlTakeoverNo"_s)));
        keepPictureOf(window, u"control-takeover-small"_s);
        clickOn(window, confirm);
        QTRY_COMPARE(answers.size(), 2);
        QVERIFY(answers.last().first().toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void keyboardAndMouseOfAMachineCanBeTakenWhereThatIsPossibleAndNowhereElse()
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
        // On the wall itself there is no such button, whatever the view says.
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        QCOMPARE(seenNamed(window, u"enlargedControl"_s), 0);
        QVERIFY(!says(window->contentItem(), u"Control"_s));

        // One machine large, and they cannot be taken: no button.
        m_control.show({});
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s) != nullptr);
        const QQuickItem* button = theOne(window, u"enlargedControl"_s);
        QVERIFY(!button->isVisible());
        // They can: "Control", in the bar, before "Actions".
        m_control.show(shown);
        QTRY_VERIFY(button->isVisible());
        QVERIFY(says(button, u"Control"_s));
        const QQuickItem* actionsButton = theOne(window, u"enlargedActions"_s);
        QVERIFY(actionsButton != nullptr && actionsButton->isVisible());
        QTRY_VERIFY(button->x() > 0 && button->x() + button->width() <= actionsButton->x());
        QCOMPARE(button->height(), actionsButton->height());
        QVERIFY(isInside(window, button));
        // Nothing of what the bar says while they are taken is there.
        QCOMPARE(seenNamed(window, u"controlGoesTo"_s), 0);
        QCOMPARE(seenNamed(window, u"controlWayBack"_s), 0);
        QCOMPARE(seenNamed(window, u"controlGiveBack"_s), 0);
        QCOMPARE(seenNamed(window, u"controlCtrlAltDel"_s), 0);
        QCOMPARE(seenNamed(window, u"controlFrame"_s), 0);
        keepPictureOf(window, u"enlarged-control"_s);

        // Clicked, it asks for them, once: while the machine is asked it waits.
        const QSignalSpy takeAsked(&m_control, &qkvm::ui::ControlView::takeAsked);
        clickOn(window, button);
        QTRY_COMPARE(takeAsked.count(), 1);
        shown.asking = true;
        m_control.show(shown);
        QTRY_VERIFY(says(button, u"Asking..."_s));
        QVERIFY(button->isVisible());
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        // The keyboard takes them as well: the button is one that Tab comes to.
        shown.asking = false;
        m_control.show(shown);
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(tabTo(window->contentItem(), u"enlargedControl"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(takeAsked.count(), 2);

        // On another machine as on this one; and gone with the machine, and with what made it possible.
        wall.stepEnlarged(1);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        m_control.show({});
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 0);
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        wall.closeEnlarged();
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s) == nullptr);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theConsoleHeaderStaysWhileControlChanges()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr && bringUp(window));
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_VERIFY(seenNamed(window, u"enlargedName"_s) == 1);
        letTheWallComeToRest(window);
        const auto* name = theOne(window, u"enlargedName"_s);
        const auto before = name->mapRectToScene(name->boundingRect());
        shown.on = true;
        m_control.show(shown);
        letTheWallComeToRest(window);
        QVERIFY(name->isVisible());
        QCOMPARE(name->mapRectToScene(name->boundingRect()), before);
        QCOMPARE(seenNamed(window, u"enlargedActions"_s), 1);
        QCOMPARE(seenNamed(window, u"enlargedClose"_s), 1);
        QCOMPARE(seenNamed(window, u"consoleStatus"_s), 1);
        connect(&m_control, &qkvm::ui::ControlView::localInputAsked, &engine, [&](bool local) {
            shown.localInput = shown.on && local;
            m_control.show(shown);
        });
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(m_control.localInput());
        QVERIFY(m_control.isOn());
        actions.showMenu({});
        QTRY_VERIFY(!m_control.localInput());
        clickOn(window, theOne(window, u"controlKeyboard"_s));
        QTRY_VERIFY(m_control.localInput());
        actions.showMenu(menuOfAKvm());
        QTRY_VERIFY(window->property("boxOpen").toBool());
        actions.showMenu({});
        QTRY_VERIFY(!window->property("boxOpen").toBool());
        QVERIFY(m_control.localInput()); // The floating keyboard still needs local input.
        clickOn(window, theOne(window, u"keyboardClose"_s));
        QTRY_VERIFY(!m_control.localInput());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theConsoleHeaderIsThePagesInBothLooks_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<QString>("theme");
        for (const int width : {640, 800, 1280}) {
            for (const QString& theme : {u"dark"_s, u"light"_s}) {
                QTest::addRow("%d wide, %s", width, qPrintable(theme)) << width << theme;
            }
        }
    }

    // The header of a machine shown large, as the page's (index.html, "focus-bar"): the machine at
    // the left, the tools packed at the right beside Close; Open console and Fullscreen with their
    // words, in the row where it has room for them, and in More otherwise. Control's words say where
    // the machine's keyboard and mouse are, as the page's do (app.js, the label of "focus-control"):
    // Control; Asking..., which a click again stops; Take over from whoever has them, where that
    // may be done; Controlled by whoever has them, where it may not. In each, Control, Actions and
    // Close stand whole in the window.
    void theConsoleHeaderIsThePagesInBothLooks()
    {
        QFETCH(int, width);
        QFETCH(QString, theme);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        QVERIFY(colours->setProperty(
            "scheme", static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark)));
        window->resize(width, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        // (A changed button is laid out with a frame, and its row with the frame after: the row
        // is looked at, and drawn, once both have come.)
        const auto drawn = [&] {
            for (int frame = 0; frame < 3; ++frame) {
                QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
                window->update();
                static_cast<void>(swapped.wait(2000));
            }
        };
        // (A machine whose own console can be opened: the page's "Open console".)
        QVERIFY(tiles[1].openable);
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        letTheWallComeToRest(window);
        // What a button of the header says, where its words fit inside it; nothing otherwise.
        const auto label = [&](const QString& name) {
            const QQuickItem* button = theOneSeen(window, name);
            if (button == nullptr) {
                return QString();
            }
            const QString words = button->property("text").toString();
            const QQuickItem* said = saying(button, words);
            const qreal room = button->width() - 24;
            return said != nullptr && said->implicitWidth() <= room + 0.5 ? words : u"(cut)"_s;
        };

        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        QCOMPARE(label(u"enlargedControl"_s), u"Control"_s);
        // The machine has what the tools leave it: in the widest window, more than the 340 px
        // it was held to.
        const QQuickItem* row = theOne(window, u"consoleHeaderRow"_s);
        const QQuickItem* machine = theOne(window, u"enlargedName"_s)->parentItem();
        if (width == 1280) {
            QVERIFY2(machine->width() > 400, qPrintable(QString::number(machine->width())));
        }
        // Open console and Fullscreen: with their words, in the row or in More.
        for (const auto& [name, words] : {std::pair{u"enlargedConsole"_s, u"Open console"_s},
                                          std::pair{u"enlargedFullscreen"_s, u"Fullscreen"_s}}) {
            // (Wherever it is: More's list is not in the window while More is closed.)
            const auto* button = window->findChild<QQuickItem*>(name);
            QVERIFY2(button != nullptr, qPrintable(name));
            QCOMPARE(button->property("text").toString(), words);
            if (row->isAncestorOf(button)) {
                QVERIFY2(isSeen(button) && says(button, words), qPrintable(name));
            } else {
                QVERIFY2(width < 1280, qPrintable(name));
                QVERIFY(revealConsoleItem(window, name));
                QVERIFY2(says(theOne(window, name), words), qPrintable(name));
                QTest::keyClick(window, Qt::Key_Escape);
                QTRY_COMPARE(window->findChild<QObject*>(u"consoleMore"_s)->property("visible").toBool(), false);
            }
        }
        keepPictureOf(window, u"console-header-%1-%2"_s.arg(width).arg(theme));
        // More, where it is there, opens on a click and closes on a second, as the bar's drops and
        // the page's do; it does not open again.
        if (const QQuickItem* moreButton = theOneSeen(window, u"consoleMoreButton"_s); moreButton != nullptr) {
            const QObject* moreMenu = window->findChild<QObject*>(u"consoleMore"_s);
            QVERIFY(moreMenu != nullptr);
            clickOn(window, moreButton);
            QTRY_VERIFY(moreMenu->property("visible").toBool());
            clickOn(window, moreButton);
            QTRY_VERIFY(!moreMenu->property("visible").toBool());
            QTest::qWait(150);
            QVERIFY(!moreMenu->property("visible").toBool());
        }

        // In the page's order (index.html, "focus-bar"): Control, Actions, Open console, Fullscreen.
        QVERIFY(theToolsAreInThePagesOrder(window));
        // And where each stands: nothing of it moves while Control's words change, for whoever has
        // the keyboard and mouse (here nobody), and a second click lands where the first did.
        const QStringList nobodysPlaces = whereTheToolsStand(window);

        // Asked for: Asking..., Control itself; a click again stops the asking.
        shown.asking = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Asking..."_s);
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        QCOMPARE(whereTheToolsStand(window), nobodysPlaces);
        keepPictureOf(window, u"console-header-asking-%1-%2"_s.arg(width).arg(theme));
        const QSignalSpy givenBack(&m_control, &qkvm::ui::ControlView::giveBackAsked);
        clickOn(window, theOneSeen(window, u"enlargedControl"_s));
        QTRY_COMPARE(givenBack.count(), 1);
        shown.asking = false;

        // Somebody else has them, and they may be taken over: whom from, asked first.
        shown.ownerName = u"anna"_s;
        shown.takeoverPossible = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Take over from anna"_s);
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        QVERIFY(theToolsAreInThePagesOrder(window));
        const QStringList annasPlaces = whereTheToolsStand(window);
        keepPictureOf(window, u"console-header-takeover-%1-%2"_s.arg(width).arg(theme));
        // Somebody else has them, and they may not be: who.
        shown.takeoverPossible = false;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Controlled by anna"_s);
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        QCOMPARE(whereTheToolsStand(window), annasPlaces);
        // Clicked, it asks: Asking..., where "Controlled by anna" stood, and as wide; Open console
        // does not come under the pointer for the click that stops it.
        shown.asking = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Asking..."_s);
        QCOMPARE(whereTheToolsStand(window), annasPlaces);
        shown.asking = false;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Controlled by anna"_s);
        QCOMPARE(whereTheToolsStand(window), annasPlaces);
        // Asked again, anna has let go meanwhile, and the keys come here: the pressed Control
        // stands where "Controlled by anna" stood, as wide (the machine forgets who held them
        // then), so that the click that gives them back lands on it and not on Next. And given
        // back, Control keeps that place too: nothing moves while this machine is shown.
        shown.asking = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Asking..."_s);
        shown.asking = false;
        shown.ownerName.clear();
        shown.on = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"controlGiveBack"_s), u"Control"_s);
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        QCOMPARE(whereTheToolsStand(window), annasPlaces);
        shown.on = false;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Control"_s);
        QCOMPARE(whereTheToolsStand(window), annasPlaces);
        // A name of any length leaves the button its place.
        shown.ownerName = u"anna"_s;
        shown.ownerName = QString(200, u'W');
        m_control.show(shown);
        drawn();
        QTRY_VERIFY(label(u"enlargedControl"_s).startsWith(u"Controlled by WWW"_s));
        QVERIFY(label(u"enlargedControl"_s).size() < 60);
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        // (Whole, the name is in the button's hint, for whoever cannot see it cut.)
        QCOMPARE(theOne(window, u"enlargedControl"_s)->property("hint").toString(),
                 u"Controlled by "_s + QString(200, u'W'));
        const QStringList widestPlaces = whereTheToolsStand(window);
        // This PC's other window has them: Control (the bar below says where they are), where it
        // stood. Taken here: Control, pressed, as wide and where it was.
        shown.ownerName = u"anna"_s;
        shown.ownerInAnotherWindow = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(label(u"enlargedControl"_s), u"Control"_s);
        QTRY_COMPARE(whereTheToolsStand(window), widestPlaces);
        shown.ownerName.clear();
        shown.ownerInAnotherWindow = false;
        shown.on = true;
        m_control.show(shown);
        drawn();
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QCOMPARE(label(u"controlGiveBack"_s), u"Control"_s);
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        QCOMPARE(whereTheToolsStand(window), widestPlaces);
        QVERIFY(theToolsAreInThePagesOrder(window));
        // Another machine shown large: its Control as wide as its words again, and the tools where
        // they stood for this one where nobody held its keys.
        shown = {};
        shown.possible = true;
        m_control.show(shown);
        wall.stepEnlarged(1);
        wall.stepEnlarged(-1);
        drawn();
        QTRY_COMPARE(whereTheToolsStand(window), nobodysPlaces);
        // Full screen: Fullscreen keeps its words, as the page's does (index.html, "focus-fullscreen"),
        // and says the way back to whoever cannot see its symbol.
        shown = {};
        shown.possible = true;
        m_control.show(shown);
        window->showFullScreen();
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        const QQuickItem* full = window->findChild<QQuickItem*>(u"enlargedFullscreen"_s);
        QVERIFY(full != nullptr);
        QTRY_COMPARE(full->property("hint").toString(), u"Leave fullscreen"_s);
        QCOMPARE(full->property("text").toString(), u"Fullscreen"_s);
        window->showNormal();
        QTRY_COMPARE(full->property("hint").toString(), u"Fullscreen"_s);
        QCOMPARE(full->property("text").toString(), u"Fullscreen"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void tabGoesThroughTheHeaderAsItIsSeen_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("before");
        QTest::addRow("640 wide") << 640 << 0;
        QTest::addRow("800 wide") << 800 << 0;
        QTest::addRow("1280 wide") << 1280 << 0;
        // (Its tools went into More at 640 and came back: put back after all the others, Tab went
        // to them after Close.)
        QTest::addRow("1280 wide, after 640") << 1280 << 640;
    }

    // Tab goes through the header's tools as they are seen, from the left: the row's in the page's
    // order, More, and Close.
    void tabGoesThroughTheHeaderAsItIsSeen()
    {
        QFETCH(int, width);
        QFETCH(int, before);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(before != 0 ? before : width, 500);
        QVERIFY(bringUp(window));
        QVERIFY(tiles[1].openable);
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        letTheWallComeToRest(window);
        if (before != 0) {
            QTRY_VERIFY(isSeen(theOne(window, u"consoleMoreButton"_s)));
            // (With the keys on Control, which keep there while the row is put in order again.)
            QVERIFY(tabTo(window->contentItem(), u"enlargedControl"_s));
            window->resize(width, 500);
        }
        QTRY_COMPARE(window->width(), width);
        // (The row is fitted a turn after it was laid out for its width, and laid out again after
        // that: three frames.)
        for (int frame = 0; frame < 3; ++frame) {
            QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
            window->update();
            static_cast<void>(swapped.wait(2000));
        }
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        if (before != 0) {
            QCOMPARE(window->activeFocusItem(), theOne(window, u"enlargedControl"_s));
        }
        // What is seen in the row, from the left.
        const auto* row = theOne(window, u"consoleHeaderRow"_s);
        QList<const QQuickItem*> seen;
        for (const QString& name : theHeadersTools()) {
            const QQuickItem* item = theHeadersTool(window, name);
            if (item != nullptr && isSeen(item) && row->isAncestorOf(item)) {
                seen.append(item);
            }
        }
        std::ranges::sort(seen, {}, [](const QQuickItem* item) { return item->mapToScene(QPointF(0, 0)).x(); });
        QVERIFY(seen.size() >= 4);
        QStringList asSeen;
        for (const QQuickItem* item : seen) {
            asSeen.append(item->objectName());
        }
        // Tab from the first of them, as far as the last.
        QVERIFY(tabTo(window->contentItem(), asSeen.constFirst()));
        QStringList byTab{window->activeFocusItem() != nullptr ? window->activeFocusItem()->objectName() : QString()};
        while (byTab.size() < asSeen.size()) {
            QTest::keyClick(window, Qt::Key_Tab);
            const QQuickItem* at = window->activeFocusItem();
            byTab.append(at != nullptr ? at->objectName() : QString());
        }
        QVERIFY2(byTab == asSeen,
                 qPrintable(u"by Tab: %1; as seen: %2"_s.arg(byTab.join(u", "_s), asSeen.join(u", "_s))));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Where something leaves the row and what was in More comes back, the row is put in the order
    // it is seen while a tool of it has the keys: they stay on that tool, outlined as they were. (At
    // 700 px Previous, Next and Fullscreen are in More; without Actions they all have room.)
    void theKeysStayOnAToolWhileTheRowIsPutInOrder()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(700, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), 700);
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        letTheWallComeToRest(window);
        QTRY_VERIFY(isSeen(theOne(window, u"consoleMoreButton"_s)));
        QCOMPARE(seenNamed(window, u"enlargedFullscreen"_s), 0);
        QVERIFY(tabTo(window->contentItem(), u"enlargedControl"_s));
        const QQuickItem* control = theOne(window, u"enlargedControl"_s);
        QVERIFY(control->property("visualFocus").toBool());
        actions.setAllowed(false);
        QTRY_COMPARE(seenNamed(window, u"enlargedFullscreen"_s), 1);
        QTRY_VERIFY(!isSeen(theOne(window, u"consoleMoreButton"_s)));
        QCOMPARE(window->activeFocusItem(), control);
        QVERIFY(control->property("visualFocus").toBool());
        // And Tab goes on from it as the row is seen: Open console, Fullscreen, Close.
        for (const QString& next : {u"enlargedConsole"_s, u"enlargedFullscreen"_s, u"enlargedClose"_s}) {
            QTest::keyClick(window, Qt::Key_Tab);
            QTRY_COMPARE(window->activeFocusItem()->objectName(), next);
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Control is one button, as the page's: Space on it asks, and it says Asking... with the keys
    // still on it, so that Space again stops the asking. (Two buttons, one for each, hid the one
    // that had the keys: they left the header, and a second Space did nothing.)
    void theControlButtonKeepsTheKeysWhileItAsks()
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
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        letTheWallComeToRest(window);
        const QQuickItem* button = theOne(window, u"enlargedControl"_s);
        const QSignalSpy takeAsked(&m_control, &qkvm::ui::ControlView::takeAsked);
        const QSignalSpy giveBackAsked(&m_control, &qkvm::ui::ControlView::giveBackAsked);
        // What came to the button and its window between the keys going there and the asking: said where Space
        // asks nothing. (Gates 34 and 36 saw that, under load, once each; 73 runs on the slow cores beside a build
        // did not, and saw nothing come between.)
        const QSignalSpy canceled(button, SIGNAL(canceled()));
        const QSignalSpy pressings(button, SIGNAL(pressedChanged()));
        const QSignalSpy clicks(button, SIGNAL(clicked()));
        const QSignalSpy keys(button, &QQuickItem::activeFocusChanged);
        const QSignalSpy activations(window, &QWindow::activeChanged);
        // From the keyboard: the machine is asked, and the button says so, the keys on it.
        QVERIFY(tabTo(window->contentItem(), u"enlargedControl"_s));
        QTRY_VERIFY(button->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        const auto between = [&] {
            const QQuickItem* now = window->activeFocusItem();
            return u"Space on Control asked nothing: pressed and let go %1 times, clicked %2, canceled %3; the keys "
                   u"came and went %4 times, the window was made active or not %5 times (active now: %6), the keys "
                   u"are at %7"_s.arg(pressings.count())
                       .arg(clicks.count())
                       .arg(canceled.count())
                       .arg(keys.count())
                       .arg(activations.count())
                       .arg(window->isActive())
                       .arg(now == nullptr ? u"nothing"_s : now->objectName());
        };
        QVERIFY2(QTest::qWaitFor([&] { return takeAsked.count() == 1; }), qPrintable(between()));
        QCOMPARE(takeAsked.count(), 1);
        shown.asking = true;
        m_control.show(shown);
        QTRY_VERIFY(says(button, u"Asking..."_s));
        QCOMPARE(theOne(window, u"enlargedControl"_s), button);
        QVERIFY(button->hasActiveFocus());
        QVERIFY(button->property("visualFocus").toBool());
        // Space again stops the asking, as a click on it does: asked to stop, nothing asked again,
        // and no key said to have been pressed before the machine answered.
        const QSignalSpy wentOn(&m_control, &qkvm::ui::ControlView::wentOnSaid);
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(giveBackAsked.count(), 1);
        QCOMPARE(takeAsked.count(), 1);
        QVERIFY2(std::ranges::none_of(wentOn, [](const QList<QVariant>& said) { return said.first().toBool(); }),
                 "Space on Asking... was said to be a key pressed before the machine answered");
        // Stopped: the machine owes its word for the asking now (as the program's control has it,
        // busy for as long as it may take), and Control is Control again, resting: it asks
        // nothing, and the keys stay on it, outlined, so that the console's keys still work.
        shown.asking = false;
        shown.busy = true;
        m_control.show(shown);
        QTRY_VERIFY(says(button, u"Control"_s));
        QTRY_VERIFY(button->property("resting").toBool());
        QCOMPARE(window->activeFocusItem(), button);
        QVERIFY(button->property("visualFocus").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        QCOMPARE(window->activeFocusItem(), button);
        // Its word has come: Space asks again.
        shown.busy = false;
        m_control.show(shown);
        QTRY_VERIFY(!button->property("resting").toBool());
        QVERIFY(button->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(takeAsked.count(), 2);
        // And the console's own keys go on working from it: Right shows the next machine.
        const QString before = wall.enlarged();
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_VERIFY(wall.enlarged() != before);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theCombinedDetailsStayReadableAndClearOnRetarget_data()
    {
        QTest::addColumn<QString>("theme");
        QTest::addColumn<int>("width");
        QTest::newRow("dark-wide") << u"dark"_s << 1280;
        QTest::newRow("light-wide") << u"light"_s << 1280;
        QTest::newRow("dark-narrow") << u"dark"_s << 640;
        QTest::newRow("light-narrow") << u"light"_s << 640;
    }

    void theCombinedDetailsStayReadableAndClearOnRetarget()
    {
        QFETCH(QString, theme);
        QFETCH(int, width);
        using qkvm::core::testing::Answered;
        using qkvm::core::testing::Asked;
        qkvm::core::testing::StandIn server([](const Asked& asked) {
            if (asked.path.startsWith("/api/v1/audit"))
                return Answered(
                    200,
                    R"({"entries":[{"time":150,"user":"administrator","event":"action","name":"Restart"},{"time":149,"user":"operator","event":"live_control","on":true},{"time":148,"user":"operator","event":"live_control","on":false},{"time":147,"user":"administrator","event":"action","name":"Power on"}],"more":true,"missing":null})");
            if (asked.path.contains("what=samples"))
                return Answered(
                    200,
                    R"({"since":100,"until":200,"every":900,"series":[{"sensor":"CPU","unit":"C","points":[[100,40,38,42],[150,41,40,44]]}],"more":false})");
            return Answered(
                200,
                R"({"since":100,"until":200,"first_seen":110,"last_seen":199,"state":"online","watched":80,"online":60,"share":0.75,"changes":2,"events":3,"sensors":["CPU"]})");
        });
        qkvm::core::ServerClient client;
        QVERIFY(client.setAddress(server.address()));
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        const auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        colours->setProperty("scheme",
                             static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark));
        window->resize(width, 700);
        QVERIFY(bringUp(window));
        wall.enlarge(tiles[1].key);
        wall.details()->bind(&client, u"machine"_s, u"admin"_s, true, true);
        clickOn(window, theOne(window, u"enlargedVideoDetails"_s));
        QTRY_VERIFY(wall.details()->opened());
        QTRY_VERIFY(!wall.details()->busy());
        QCOMPARE(wall.details()->graphs().size(), 1);
        const auto* panel = theOne(window, u"videoDiagnostics"_s);
        QVERIFY(isInside(window, panel));
        QVERIFY(theOne(window, u"consoleHistorySummary"_s)->property("text").toString().contains(u"75 %"_s));
        QVERIFY(theOne(window, u"consoleActivity"_s)->isVisible());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
        keepPictureOf(window, u"console-details-%1-%2"_s.arg(width).arg(theme));
        auto* scroll = theOne(window, u"consoleDetailsScroll"_s);
        QVERIFY(tabTo(window->contentItem(), u"consoleDetailsScroll"_s));
        // (Once what the details hold is taller than they are: on a slow machine it was not yet, in the wide
        // window, and End had nowhere to go.)
        QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height());
        QTest::keyClick(window, Qt::Key_End);
        QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
        letTheWallComeToRest(window);
        // End moves at once; what the details hold may still grow after it (on a slow machine the button
        // was then below the window, and the click went past it). So End once more, once all has come to
        // rest, and the button within the scroll's own bounds, which clip what lies outside them.
        QVERIFY(scroll->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_End);
        const auto* more = theOne(window, u"moreConsoleActivity"_s);
        QTRY_VERIFY(isInside(window, more) && isWithin(scroll, more));
        clickOn(window, more);
        QVERIFY(panel->property("allActivity").toBool());
        keepPictureOf(window, u"console-activity-%1-%2"_s.arg(width).arg(theme));
        wall.details()->bind(&client, u"machine"_s, u"viewer"_s, false, true);
        QVERIFY(wall.details()->activity().isEmpty());
        QTRY_COMPARE(seenNamed(window, u"consoleActivity"_s), 0);
        wall.enlarge(tiles[2].key);
        QTRY_VERIFY(!wall.details()->opened());
        QTRY_COMPARE(seenNamed(window, u"videoDiagnostics"_s), 0);
        QVERIFY(wall.details()->summary().isEmpty());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theFloatingKeyboardAndOverflowStayUsable_data()
    {
        QTest::addColumn<QString>("theme");
        QTest::newRow("dark") << u"dark"_s;
        QTest::newRow("light") << u"light"_s;
    }

    void theFloatingKeyboardAndOverflowStayUsable()
    {
        QFETCH(QString, theme);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        m_looks.back()->choose(theme);
        auto* colours = engine.singletonInstance<QObject*>("Qkvm", "Theme");
        QVERIFY(colours != nullptr);
        colours->setProperty("scheme",
                             static_cast<int>(theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark));
        QVERIFY(bringUp(window));
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        connect(&m_control, &qkvm::ui::ControlView::localInputAsked, &engine, [&](bool local) {
            shown.localInput = shown.on && local;
            m_control.show(shown);
        });
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlKeyboard"_s), 1);
        clickOn(window, theOne(window, u"controlKeyboard"_s));
        QTRY_COMPARE(seenNamed(window, u"floatingKeyboard"_s), 1);
        QVERIFY(!theOne(window, u"screenKey:4"_s)->isEnabled());
        QVERIFY(!m_control.localInput());
        // Open before taking relative control, while the local pointer is still free.
        shown.relative = true;
        shown.on = true;
        m_control.show(shown);
        QTRY_VERIFY(theOne(window, u"screenKey:4"_s)->isEnabled());
        QVERIFY(m_control.localInput());
        const QSignalSpy sent(&m_control, &qkvm::ui::ControlView::keysTapped);
        clickOn(window, theOne(window, u"screenKey:225"_s));
        QCOMPARE(sent.size(), 0); // Arming a modifier is local only.
        QVERIFY(theOne(window, u"screenKey:225"_s)->property("checked").toBool());
        clickOn(window, theOne(window, u"screenKey:4"_s));
        QCOMPARE(sent.size(), 1);
        QCOMPARE(qvariant_cast<QList<int>>(sent.first().first()), QList<int>({225, 4}));
        QVERIFY(!theOne(window, u"screenKey:225"_s)->property("checked").toBool());
        clickOn(window, theOne(window, u"screenKey:4"_s));
        QCOMPARE(qvariant_cast<QList<int>>(sent.last().first()), QList<int>({4}));
        const auto* keyboard = theOne(window, u"floatingKeyboard"_s);
        const auto* title = theOne(window, u"keyboardDrag"_s);
        const QPoint start = title->mapToScene(QPointF(40, title->height() / 2)).toPoint();
        const QPointF before = keyboard->position();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(35, -35), 30);
        QTest::mouseMove(window, start + QPoint(70, -70), 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(70, -70));
        QTRY_VERIFY(keyboard->position() != before);
        QVERIFY(isInside(window, keyboard));
        QVERIFY(tabTo(window->contentItem(), u"keyboardResize"_s));
        const qreal oldWidth = keyboard->width();
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_VERIFY(keyboard->width() < oldWidth);
        QCOMPARE(sent.size(), 2); // Moving or resizing sends no remote key.
        clickOn(window, theOne(window, u"keyboardReset"_s));
        for (const QSize size : {QSize(1280, 800), QSize(800, 500), QSize(640, 400)}) {
            window->resize(size);
            QTRY_VERIFY(window->size() == size && isInside(window, keyboard));
            const auto* header = theOne(window, u"consoleHeader"_s);
            const auto* status = theOne(window, u"consoleStatus"_s);
            QTRY_VERIFY(keyboard->mapToScene(QPointF()).y() >= header->mapToScene(QPointF(0, header->height())).y());
            QTRY_VERIFY(keyboard->mapToScene(QPointF(0, keyboard->height())).y() <= status->mapToScene(QPointF()).y());
            QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
            keepPictureOf(window, u"console-keyboard-%1-%2"_s.arg(size.width()).arg(theme));
        }
        // Tabbing to a clipped key must bring it fully into the scroll viewport.
        QVERIFY(tabTo(window->contentItem(), u"screenKey:70"_s));
        const auto* lastKey = theOne(window, u"screenKey:70"_s);
        const auto* body = theOne(window, u"keyboardBody"_s);
        QTRY_VERIFY(body->mapRectToScene(body->boundingRect())
                        .adjusted(-1, -1, 1, 1)
                        .contains(lastKey->mapRectToScene(lastKey->boundingRect())));
        QVERIFY(says(window->contentItem(), u"Relative mouse is paused while local controls are open."_s));
        clickOn(window, theOne(window, u"keyboardClose"_s));
        QTRY_COMPARE(seenNamed(window, u"floatingKeyboard"_s), 0);
        QVERIFY(!m_control.localInput());
        QVERIFY(revealConsoleItem(window, u"controlAltF4"_s));
        QVERIFY(m_control.localInput());
        keepPictureOf(window, u"console-input-more-%1"_s.arg(theme));
        clickOn(window, theOne(window, u"controlAltF4"_s));
        // (Asked first, as on the page: ctrlAltDelAndAltF4AreAskedFirstNamingTheMachine. The keys
        // stay this PC's while it is asked.)
        const QObject* question = window->findChild<QObject*>(u"controlKeysQuestion"_s);
        QVERIFY(question != nullptr);
        QTRY_VERIFY(question->property("opened").toBool());
        QCOMPARE(sent.size(), 2);
        QTRY_VERIFY(m_control.localInput());
        clickOn(window, theOne(window, u"controlKeysYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(sent.size(), 3);
        QCOMPARE(qvariant_cast<QList<int>>(sent.last().first()), QList<int>({226, 61}));
        QTRY_VERIFY(!m_control.localInput());
        // Wider translated controls must move intact into the header's own More menu.
        auto* next = window->findChild<QQuickItem*>(u"enlargedNext"_s);
        QVERIFY(next != nullptr);
        next->setImplicitWidth(110);
        QTRY_COMPARE(seenNamed(window, u"consoleMoreButton"_s), 1);
        QVERIFY(revealConsoleItem(window, u"enlargedNext"_s));
        QVERIFY(m_control.localInput());
        keepPictureOf(window, u"console-header-more-%1"_s.arg(theme));
        clickOn(window, theOne(window, u"enlargedNext"_s));
        QTRY_COMPARE(wall.enlarged(), tiles[2].key);
        // Closing control clears the panel and every locally armed modifier.
        window->resize(1280, 800);
        QVERIFY(revealConsoleItem(window, u"controlKeyboard"_s));
        clickOn(window, theOne(window, u"controlKeyboard"_s));
        QTRY_COMPARE(seenNamed(window, u"floatingKeyboard"_s), 1);
        QVERIFY(tabTo(window->contentItem(), u"screenKey:225"_s));
        QTRY_VERIFY(body->mapRectToScene(body->boundingRect())
                        .adjusted(-1, -1, 1, 1)
                        .contains(theOne(window, u"screenKey:225"_s)
                                      ->mapRectToScene(theOne(window, u"screenKey:225"_s)->boundingRect())));
        clickOn(window, theOne(window, u"screenKey:225"_s));
        QVERIFY(theOne(window, u"screenKey:225"_s)->property("checked").toBool());
        shown.on = false;
        shown.localInput = false;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"floatingKeyboard"_s), 0);
        shown.on = true;
        m_control.show(shown);
        clickOn(window, theOne(window, u"controlKeyboard"_s));
        QTRY_VERIFY(!theOne(window, u"screenKey:225"_s)->property("checked").toBool());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whileKeyboardAndMouseAreTakenTheBarSaysSoAndHowTheyAreGivenBack()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr && bringUp(window));
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        m_control.show(shown);

        QTRY_COMPARE(seenNamed(window, u"controlGoesTo"_s), 1);
        QVERIFY(says(window->contentItem(), u"Controlling"_s));
        for (const QString& name :
             {u"enlargedName"_s, u"enlargedActions"_s, u"enlargedClose"_s, u"controlGiveBack"_s, u"controlCtrlAltDel"_s,
              u"controlTypeText"_s, u"controlWayBack"_s, u"consoleStatus"_s}) {
            QTRY_COMPARE(seenNamed(window, name), 1);
            // (Once the header has laid its row out, a frame after the change.)
            QTRY_VERIFY(isInside(window, theOne(window, name)));
        }
        const auto* name = theOne(window, u"enlargedName"_s);
        QCOMPARE(name->property("text").toString(), tiles[1].name);
        const auto* frame = theOne(window, u"controlFrame"_s);
        const auto* screen = theOne(window, u"enlargedScreen"_s);
        QVERIFY(frame != nullptr && screen != nullptr && frame->isVisible());
        QCOMPARE(frame->mapRectToScene(frame->boundingRect()), screen->mapRectToScene(screen->boundingRect()));
        QCOMPARE(frame->property("border").value<QObject*>()->property("color").value<QColor>(), QColor(u"#1d4ed8"_s));
        QVERIFY(frame->property("border").value<QObject*>()->property("width").toInt() >= 2);
        QVERIFY(theOne(window, u"controlWayBack"_s)->property("text").toString().contains(u"Right Ctrl"_s));
        const QSignalSpy textAsked(&actions, &qkvm::ui::ActionsView::textFormAsked);
        clickOn(window, theOne(window, u"controlTypeText"_s));
        QCOMPARE(textAsked.size(), 1);
        QCOMPARE(textAsked.first().first().toString(), wall.enlarged());
        const QSignalSpy givenBack(&m_control, &qkvm::ui::ControlView::giveBackAsked);
        const QSignalSpy cad(&m_control, &qkvm::ui::ControlView::ctrlAltDelAsked);
        auto* focus = window->activeFocusItem();
        // (Asked first, as on the page: ctrlAltDelAndAltF4AreAskedFirstNamingTheMachine.)
        clickOn(window, theOne(window, u"controlCtrlAltDel"_s));
        const QObject* question = window->findChild<QObject*>(u"controlKeysQuestion"_s);
        QVERIFY(question != nullptr);
        QTRY_VERIFY(question->property("opened").toBool());
        QCOMPARE(cad.size(), 0);
        clickOn(window, theOne(window, u"controlKeysYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(cad.size(), 1);
        QCOMPARE(givenBack.size(), 0);
        clickOn(window, theOne(window, u"controlGiveBack"_s));
        QCOMPARE(givenBack.size(), 1);
        QCOMPARE(window->activeFocusItem(), focus);
        keepPictureOf(window, u"enlarged-controlled"_s);
        window->resize(640, 400);
        QTRY_VERIFY(isInside(window, theOne(window, u"controlGiveBack"_s)));
        QTRY_VERIFY(isInside(window, theOne(window, u"consoleStatus"_s)));
        keepPictureOf(window, u"enlarged-controlled-narrow"_s);
        shown.on = false;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGoesTo"_s), 0);
        QCOMPARE(seenNamed(window, u"controlGiveBack"_s), 0);
        QCOMPARE(seenNamed(window, u"controlFrame"_s), 0);
        QCOMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        QCOMPARE(seenNamed(window, u"enlargedName"_s), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // Ctrl+Alt+Del and Alt+F4 are asked first, as the page asks them, the machine named; nothing
    // goes before the yes, Esc and Cancel send nothing, and the yes sends them once. While it is
    // asked the keys are this PC's (what was held is let go of then: test_control has it), and
    // the machine's again after. Where keyboard and mouse are given back meanwhile, or another
    // machine is shown, the question goes, and nothing is sent. Win, Alt+Tab and PrtSc go at once.
    void ctrlAltDelAndAltF4AreAskedFirstNamingTheMachine()
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
        wall.enlarge(tiles[1].key);
        QCOMPARE(tiles[1].name, u"NAS"_s);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QObject* question = window->findChild<QObject*>(u"controlKeysQuestion"_s);
        QVERIFY(question != nullptr);
        QVERIFY(!question->property("opened").toBool());
        const QSignalSpy cad(&m_control, &qkvm::ui::ControlView::ctrlAltDelAsked);
        const QSignalSpy tapped(&m_control, &qkvm::ui::ControlView::keysTapped);
        const QSignalSpy local(&m_control, &qkvm::ui::ControlView::localInputAsked);
        const auto lastLocal = [&] { return local.isEmpty() ? QVariant() : local.last().first(); };
        const auto inTheQuestion = [&](const QString& name) {
            QList<const QQuickItem*> found;
            named(question->property("contentItem").value<QQuickItem*>(), name, found);
            return found.size() == 1 ? found.constFirst() : nullptr;
        };
        const auto ask = [&](const QString& button) {
            if (!revealConsoleItem(window, button)) {
                return false;
            }
            clickOn(window, theOne(window, button));
            return QTest::qWaitFor([&] { return question->property("opened").toBool(); }, 5000);
        };

        // A name in such a sentence: set off; and a name that shows nothing is "this machine".
        QCOMPARE(qkvm::ui::ControlView::inASentence(u"NAS"_s), u"\"NAS\""_s);
        QCOMPARE(qkvm::ui::ControlView::inASentence(QString()), u"this machine"_s);
        QCOMPARE(qkvm::ui::ControlView::inASentence(u"   "_s), u"this machine"_s);

        // Ctrl+Alt+Del: asked, with the machine named and set off as a sentence sets a name off,
        // and nothing is sent. Its yes is the ordinary one. The keys are this PC's meanwhile, and
        // the keyboard starts at Cancel.
        QVERIFY(ask(u"controlCtrlAltDel"_s));
        QCOMPARE(question->property("title").toString(), u"Ctrl+Alt+Del"_s);
        QVERIFY(says(question->property("contentItem").value<QQuickItem*>(), u"Ctrl+Alt+Del on \"NAS\"?"_s));
        QCOMPARE(inTheQuestion(u"controlKeysYes"_s)->property("text").toString(), u"Ctrl+Alt+Del"_s);
        QVERIFY(inTheQuestion(u"controlKeysYes"_s)->property("primary").toBool());
        QVERIFY(!inTheQuestion(u"controlKeysYes"_s)->property("danger").toBool());
        QCOMPARE(cad.count(), 0);
        QCOMPARE(lastLocal(), QVariant(true));
        QTRY_VERIFY(window->activeFocusItem() == inTheQuestion(u"controlKeysNo"_s));
        keepPictureOf(window, u"console-ctrl-alt-del-asked"_s);
        // Esc is a no: nothing sent, and the keys are the machine's again.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!question->property("opened").toBool());
        QTest::qWait(50);
        QCOMPARE(cad.count(), 0);
        QCOMPARE(lastLocal(), QVariant(false));
        // Cancel too.
        QVERIFY(ask(u"controlCtrlAltDel"_s));
        QCOMPARE(lastLocal(), QVariant(true));
        clickOn(window, inTheQuestion(u"controlKeysNo"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(cad.count(), 0);
        QCOMPARE(lastLocal(), QVariant(false));
        // The yes sends them, once.
        QVERIFY(ask(u"controlCtrlAltDel"_s));
        clickOn(window, inTheQuestion(u"controlKeysYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(cad.count(), 1);
        QCOMPARE(lastLocal(), QVariant(false));

        // Alt+F4: asked with what it does there, and its yes is red.
        QVERIFY(ask(u"controlAltF4"_s));
        QCOMPARE(question->property("title").toString(), u"Alt+F4"_s);
        QVERIFY(says(question->property("contentItem").value<QQuickItem*>(),
                     u"Alt+F4 on \"NAS\"? It closes the window that is in front there."_s));
        QCOMPARE(inTheQuestion(u"controlKeysYes"_s)->property("text").toString(), u"Alt+F4"_s);
        QVERIFY(inTheQuestion(u"controlKeysYes"_s)->property("danger").toBool());
        QCOMPARE(tapped.count(), 0);
        keepPictureOf(window, u"console-alt-f4-asked"_s);
        clickOn(window, inTheQuestion(u"controlKeysYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(tapped.count(), 1);
        QCOMPARE(tapped.first().first().value<QList<int>>(), (QList<int>{226, 61}));
        QCOMPARE(cad.count(), 1);

        // While it is asked, the keys stay this PC's and the keyboard stays in it: when the
        // program says something new of keyboard and mouse, and when a box opens over it and
        // closes again.
        QVERIFY(ask(u"controlCtrlAltDel"_s));
        shown.note = u"Keys are sent as a US keyboard types them."_s;
        m_control.show(shown);
        QTest::qWait(50);
        QCOMPARE(lastLocal(), QVariant(true));
        QTRY_VERIFY(window->activeFocusItem() == inTheQuestion(u"controlKeysNo"_s));
        QObject* aboutBox = window->findChild<QObject*>(u"aboutBox"_s);
        QVERIFY(aboutBox != nullptr);
        QVERIFY(QMetaObject::invokeMethod(aboutBox, "open"));
        QTRY_VERIFY(aboutBox->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(aboutBox, "close"));
        QTRY_VERIFY(!aboutBox->property("opened").toBool());
        QTest::qWait(50);
        QVERIFY(question->property("opened").toBool());
        QCOMPARE(lastLocal(), QVariant(true));
        // Asked, and keyboard and mouse are given back meanwhile: the question goes, and nothing
        // is sent; nor by a yes that comes after that (as one may while the box is on its way out).
        shown.on = false;
        m_control.show(shown);
        QTRY_VERIFY(!question->property("opened").toBool());
        QObject* lateYes = question->findChild<QObject*>(u"controlKeysYes"_s);
        QVERIFY(lateYes != nullptr);
        QVERIFY(QMetaObject::invokeMethod(lateYes, "clicked"));
        QTest::qWait(50);
        QCOMPARE(cad.count(), 1);
        // Asked, and another machine is shown meanwhile: the same.
        shown.on = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QVERIFY(ask(u"controlAltF4"_s));
        wall.enlarge(tiles[2].key);
        QTRY_VERIFY(!question->property("opened").toBool());
        // (Nor by a late yes: what is shown now is not what it was asked for.)
        QVERIFY(QMetaObject::invokeMethod(lateYes, "clicked"));
        QTest::qWait(50);
        QCOMPARE(tapped.count(), 1);
        wall.enlarge(tiles[1].key);

        // Asked from the menu of the keys that do not fit, in the smallest window (with "Type
        // text" in the row as well): the keys stay this PC's from the menu to the question, and
        // do not go back to the machine between.
        shown.textAllowed = true;
        m_control.show(shown);
        window->findChild<QQuickItem*>(u"controlKeyboard"_s)->setImplicitWidth(300);
        window->resize(640, 400);
        QTRY_COMPARE(window->width(), 640);
        QTRY_COMPARE(seenNamed(window, u"inputMoreButton"_s), 1);
        // (In the menu, which is closed: not in what the window shows.)
        QTRY_VERIFY(theOne(window, u"controlAltF4"_s) == nullptr);
        QVERIFY(revealConsoleItem(window, u"controlAltF4"_s));
        QCOMPARE(lastLocal(), QVariant(true));
        const qsizetype fromTheMenu = local.count();
        clickOn(window, theOne(window, u"controlAltF4"_s));
        QTRY_VERIFY(question->property("opened").toBool());
        QTest::qWait(100);
        for (qsizetype at = fromTheMenu; at < local.count(); ++at) {
            QVERIFY2(local.at(at).first().toBool(),
                     "the keys went back to the machine between the menu and the question");
        }
        clickOn(window, inTheQuestion(u"controlKeysNo"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QCOMPARE(tapped.count(), 1);
        window->resize(1280, 800);
        QTRY_COMPARE(window->width(), 1280);

        // Win, Alt+Tab and PrtSc go at once, as on the page.
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        for (const QString& button : {u"controlWin"_s, u"controlAltTab"_s, u"controlPrintScreen"_s}) {
            const auto before = tapped.count();
            QVERIFY2(revealConsoleItem(window, button), qPrintable(button));
            clickOn(window, theOne(window, button));
            QTRY_COMPARE(tapped.count(), before + 1);
            QVERIFY(!question->property("opened").toBool());
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // F fills the screen with the window and gives it back, as the page's F does (F11 stays); and
    // C, where a machine is shown large, does what its Control button does: takes its keyboard and
    // mouse, asks to take them over, or stops asking. Where something is typed a letter is a
    // letter, while a box is open the keys are the box's, while the keyboard on the screen is open
    // C and F are its, and while keyboard and mouse are taken every key is the machine's. Held,
    // either does what it does once.
    void theKeysFAndCDoWhatThePagesDoAndRestWhereALetterIsAnothers()
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
        QTRY_COMPARE(count<qkvm::ui::VideoTile>(window->contentItem()), 15);
        const QSignalSpy takeAsked(&m_control, &qkvm::ui::ControlView::takeAsked);
        const QSignalSpy takeoverAsked(&m_control, &qkvm::ui::ControlView::takeoverAsked);
        const QSignalSpy giveBackAsked(&m_control, &qkvm::ui::ControlView::giveBackAsked);
        QObject* settings = window->findChild<QObject*>(u"settingsBox"_s);
        QVERIFY(settings != nullptr);
        const auto fullScreenAfter = [&](Qt::Key key) {
            QTest::keyClick(window, key);
            QTest::qWait(100);
            return window->visibility() == QWindow::FullScreen;
        };
        // A key that is held: pressed once, and then said again and again by Windows, as a held
        // key is (each time as a shortcut first, as Qt asks a key's shortcut before the key).
        const auto held = [&](Qt::Key key, const QString& letter) {
            QTest::keyPress(window, key);
            for (int again = 0; again < 3; ++again) {
                if (!qt_sendShortcutOverrideEvent(window, 0, key, Qt::NoModifier, letter, true)) {
                    qt_handleKeyEvent(window, QEvent::KeyPress, key, Qt::NoModifier, letter, true);
                }
                QTest::qWait(20);
            }
            QTest::keyRelease(window, key);
            QTest::qWait(100);
        };

        // F on the wall: the whole screen, and back; held, once.
        QVERIFY(window->visibility() != QWindow::FullScreen);
        QTest::keyClick(window, Qt::Key_F);
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        QTest::keyClick(window, Qt::Key_F);
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        held(Qt::Key_F, u"f"_s);
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        QTest::qWait(100);
        QCOMPARE(window->visibility(), QWindow::FullScreen);
        QTest::keyClick(window, Qt::Key_F);
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        // Typed into "Find a KVM", a letter.
        const QQuickItem* find = theOne(window, u"findAKvm"_s);
        QVERIFY(find != nullptr);
        QTest::keyClick(window, Qt::Key_Slash);
        QTRY_VERIFY(find->hasActiveFocus());
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        QCOMPARE(find->property("text").toString(), u"f"_s);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!find->hasActiveFocus());
        // While a box is open, the box's.
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());
        // While the wall's order is made, its keys rest, as on the page.
        wall.arrange();
        QTRY_VERIFY(wall.arranging());
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        wall.stopArranging();
        QTRY_VERIFY(!wall.arranging());
        // C on the wall: no machine is shown large, and nothing is taken.
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTest::keyClick(window, Qt::Key_C);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 0);

        // A machine shown large: C takes its keyboard and mouse, as the button does; F is the
        // screen's still.
        wall.enlarge(tiles[1].key);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        // (Held, it asks once: the button is not pressed again by the key's repeat.)
        held(Qt::Key_C, u"c"_s);
        QTRY_COMPARE(takeAsked.count(), 1);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        QTest::keyClick(window, Qt::Key_F);
        QTRY_COMPARE(window->visibility(), QWindow::FullScreen);
        QTest::keyClick(window, Qt::Key_F);
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        // While they are asked for, C stops asking, as the button does then; and it is no key
        // pressed before the machine answered, which would end the asking all by itself and
        // leave C nothing to stop (the program's control says so of any other key: wentOn).
        shown.asking = true;
        m_control.show(shown);
        QTRY_VERIFY(says(theOne(window, u"enlargedControl"_s), u"Asking..."_s));
        const QSignalSpy wentOn(&m_control, &qkvm::ui::ControlView::wentOnSaid);
        QTest::keyClick(window, Qt::Key_C);
        QTRY_COMPARE(giveBackAsked.count(), 1);
        QVERIFY2(std::ranges::none_of(wentOn, [](const QList<QVariant>& said) { return said.first().toBool(); }),
                 "C was said to be a key pressed before the machine answered");
        // Where somebody else has them, C asks to take them over, as the button does (which is
        // asked first).
        shown.asking = false;
        shown.takeoverPossible = true;
        shown.ownerName = u"anna"_s;
        m_control.show(shown);
        QTest::keyClick(window, Qt::Key_C);
        QTRY_COMPARE(takeoverAsked.count(), 1);
        QCOMPARE(takeAsked.count(), 1);
        shown.takeoverPossible = false;
        shown.ownerName.clear();
        // While an earlier asking is under way, nothing, as the button is nothing then.
        shown.busy = true;
        m_control.show(shown);
        QTest::keyClick(window, Qt::Key_C);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        shown.busy = false;
        m_control.show(shown);
        // While a box is open over it, the box's.
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(settings->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_C);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!settings->property("opened").toBool());
        // While the keyboard on the screen is open, its.
        QVERIFY(revealConsoleItem(window, u"controlKeyboard"_s));
        clickOn(window, theOne(window, u"controlKeyboard"_s));
        QTRY_VERIFY(theOneSeen(window, u"floatingKeyboard"_s) != nullptr);
        QTest::keyClick(window, Qt::Key_C);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(theOneSeen(window, u"floatingKeyboard"_s) == nullptr);
        // And while its details are open, theirs.
        clickOn(window, theOne(window, u"enlargedVideoDetails"_s));
        QTRY_VERIFY(theOneSeen(window, u"videoDiagnostics"_s) != nullptr);
        QTest::keyClick(window, Qt::Key_C);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 1);
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        clickOn(window, theOne(window, u"enlargedVideoDetails"_s));
        QTRY_VERIFY(theOneSeen(window, u"videoDiagnostics"_s) == nullptr);
        QTest::keyClick(window, Qt::Key_C);
        QTRY_COMPARE(takeAsked.count(), 2);

        // Taken: every key is the machine's, F and C as any other; and so they are while a panel
        // of the screen's own has the keys back for a moment.
        shown.on = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        QTest::keyClick(window, Qt::Key_C);
        shown.localInput = true;
        m_control.show(shown);
        QVERIFY(!fullScreenAfter(Qt::Key_F));
        QTest::keyClick(window, Qt::Key_C);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 2);
        QCOMPARE(giveBackAsked.count(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // The keyboard on the screen asks before Ctrl+Alt+Del and Alt+F4 as the bar's buttons do:
    // armed Ctrl and Alt and then Delete, or armed Alt and then F4, open the same question, and
    // the chord goes only on its yes. Every other chord goes at once.
    void theKeyboardOnTheScreenAsksBeforeCtrlAltDelAndAltF4()
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
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        connect(&m_control, &qkvm::ui::ControlView::localInputAsked, &engine, [&](bool local) {
            shown.localInput = shown.on && local;
            m_control.show(shown);
        });
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QVERIFY(revealConsoleItem(window, u"controlKeyboard"_s));
        clickOn(window, theOne(window, u"controlKeyboard"_s));
        QTRY_COMPARE(seenNamed(window, u"floatingKeyboard"_s), 1);
        QObject* question = window->findChild<QObject*>(u"controlKeysQuestion"_s);
        QVERIFY(question != nullptr);
        const QSignalSpy tapped(&m_control, &qkvm::ui::ControlView::keysTapped);
        const QSignalSpy cad(&m_control, &qkvm::ui::ControlView::ctrlAltDelAsked);
        const auto key = [&](int usage) { clickOn(window, theOne(window, u"screenKey:%1"_s.arg(usage))); };
        const auto inTheQuestion = [&](const QString& name) {
            QList<const QQuickItem*> found;
            named(question->property("contentItem").value<QQuickItem*>(), name, found);
            return found.size() == 1 ? found.constFirst() : nullptr;
        };

        // Ctrl, Alt, Delete: asked, and nothing sent; Cancel sends nothing, and the modifiers are
        // let go of all the same.
        key(224);
        key(226);
        key(76);
        QTRY_VERIFY(question->property("opened").toBool());
        QCOMPARE(question->property("title").toString(), u"Ctrl+Alt+Del"_s);
        QVERIFY(says(question->property("contentItem").value<QQuickItem*>(), u"Ctrl+Alt+Del on \"NAS\"?"_s));
        QCOMPARE(tapped.count(), 0);
        QCOMPARE(cad.count(), 0);
        clickOn(window, inTheQuestion(u"controlKeysNo"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QTest::qWait(50);
        QCOMPARE(tapped.count(), 0);
        QVERIFY(!theOne(window, u"screenKey:224"_s)->property("checked").toBool());
        QVERIFY(!theOne(window, u"screenKey:226"_s)->property("checked").toBool());
        // The keyboard on the screen is still open, and the keys still this PC's.
        QCOMPARE(seenNamed(window, u"floatingKeyboard"_s), 1);
        QVERIFY(m_control.localInput());
        // Asked again, and the yes: the chord as it was pressed, once.
        key(224);
        key(226);
        key(76);
        QTRY_VERIFY(question->property("opened").toBool());
        clickOn(window, inTheQuestion(u"controlKeysYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QTRY_COMPARE(tapped.count(), 1);
        QCOMPARE(qvariant_cast<QList<int>>(tapped.last().first()), (QList<int>{224, 226, 76}));

        // Alt, F4: asked as Alt+F4, its yes red; the yes sends it.
        key(226);
        key(61);
        QTRY_VERIFY(question->property("opened").toBool());
        QCOMPARE(question->property("title").toString(), u"Alt+F4"_s);
        QVERIFY(inTheQuestion(u"controlKeysYes"_s)->property("danger").toBool());
        QCOMPARE(tapped.count(), 1);
        clickOn(window, inTheQuestion(u"controlKeysYes"_s));
        QTRY_VERIFY(!question->property("opened").toBool());
        QTRY_COMPARE(tapped.count(), 2);
        QCOMPARE(qvariant_cast<QList<int>>(tapped.last().first()), (QList<int>{226, 61}));

        // Any other chord goes at once: Ctrl+C, Shift+F4, Delete by itself, and Ctrl and Alt with
        // F4, which is no Alt+F4 (it changes the console on a Linux machine) and is not asked as one.
        key(224);
        key(6);
        QTRY_COMPARE(tapped.count(), 3);
        key(225);
        key(61);
        QTRY_COMPARE(tapped.count(), 4);
        key(76);
        QTRY_COMPARE(tapped.count(), 5);
        key(224);
        key(226);
        key(61);
        QTRY_COMPARE(tapped.count(), 6);
        QCOMPARE(qvariant_cast<QList<int>>(tapped.last().first()), (QList<int>{224, 226, 61}));
        // And asked with more armed beside them: Shift (or Win) and Alt with F4 still closes the
        // window in front, and Ctrl and Alt with Delete is that whatever else is armed.
        for (const QList<int>& chord : {QList<int>{225, 226, 61}, QList<int>{227, 226, 61},
                                        QList<int>{224, 225, 226, 76}, QList<int>{227, 224, 226, 76}}) {
            for (const int usage : chord) {
                key(usage);
            }
            QTRY_VERIFY2(question->property("opened").toBool(), qPrintable(QString::number(chord.constLast())));
            QCOMPARE(question->property("title").toString(), chord.constLast() == 61 ? u"Alt+F4"_s : u"Ctrl+Alt+Del"_s);
            QCOMPARE(tapped.count(), 6);
            clickOn(window, inTheQuestion(u"controlKeysNo"_s));
            QTRY_VERIFY(!question->property("opened").toBool());
        }
        QCOMPARE(tapped.count(), 6);
        QVERIFY(!question->property("opened").toBool());
        QCOMPARE(cad.count(), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whileKeyboardAndMouseAreTakenNoKeyDoesAnythingInTheWindow()
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
        const QSignalSpy windowAsked(window, SIGNAL(windowAsked()));
        const QSignalSpy takeAsked(&m_control, &qkvm::ui::ControlView::takeAsked);
        const QSignalSpy giveBackAsked(&m_control, &qkvm::ui::ControlView::giveBackAsked);
        const QSignalSpy ctrlAltDelAsked(&m_control, &qkvm::ui::ControlView::ctrlAltDelAsked);
        const QObject* settingsBox = window->findChild<QObject*>(u"settingsBox"_s);
        QVERIFY(settingsBox != nullptr);
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s) != nullptr);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);

        // Not taken: the window's keys are the window's. (So that what follows shows something.)
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTRY_COMPARE(windowAsked.count(), 1);
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(wall.enlarged(), tiles[2].key);

        // Taken, by the keyboard, from the button itself: the keyboard is not left on a button
        // that is gone.
        QVERIFY(tabTo(window->contentItem(), u"enlargedControl"_s));
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(takeAsked.count(), 1);
        shown.on = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        // Every key is the machine's. The keys the enlarged view has for itself, the window's own
        // shortcuts, and the keys that would work a button: none does anything here.
        for (const Qt::Key key : {Qt::Key_Escape, Qt::Key_Left, Qt::Key_Right, Qt::Key_F11, Qt::Key_Tab,
                                  Qt::Key_Backtab, Qt::Key_Space, Qt::Key_Return, Qt::Key_Enter, Qt::Key_S, Qt::Key_D,
                                  Qt::Key_Slash, Qt::Key_Delete, Qt::Key_A, Qt::Key_F, Qt::Key_C}) {
            QTest::keyClick(window, key);
        }
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Tab, Qt::ShiftModifier);
        QTest::keyClick(window, Qt::Key_Delete, Qt::ControlModifier | Qt::AltModifier);
        QTest::qWait(200);
        QCOMPARE(wall.enlarged(), tiles[2].key);
        QCOMPARE(windowAsked.count(), 1);
        QVERIFY(window->visibility() != QWindow::FullScreen);
        QVERIFY(!settingsBox->property("opened").toBool());
        QVERIFY(!wall.selecting());
        QVERIFY(!wall.displaying());
        QCOMPARE(giveBackAsked.count(), 0);
        QCOMPARE(ctrlAltDelAsked.count(), 0);
        QCOMPARE(takeAsked.count(), 1);
        QCOMPARE(seenNamed(window, u"controlGiveBack"_s), 1);

        // Given back: the keys are the window's again.
        shown.on = false;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        QTest::keyClick(window, Qt::Key_N, Qt::ControlModifier);
        QTRY_COMPARE(windowAsked.count(), 2);
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(wall.enlarged(), tiles[1].key);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_COMPARE(wall.enlarged(), QString());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whyKeyboardAndMouseWereNotGivenIsSaidInTheBar()
    {
        using qkvm::core::testing::Answered;
        using qkvm::core::testing::Asked;
        qkvm::core::testing::StandIn elsewhere([](const Asked&) { return Answered{200, "no picture", "image/png"}; });
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
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"controlProblem"_s) != nullptr);
        const QQuickItem* problem = theOne(window, u"controlProblem"_s);
        QVERIFY(!problem->isVisible());
        const QQuickItem* screen = theOne(window, u"enlargedScreen"_s);
        letTheWallComeToRest(window);
        const qreal plainTop = screen->mapToScene(QPointF(0, 0)).y();

        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.problem = u"Keys could not be passed on in time. Keyboard and mouse were given back, and every key on "
                        "the machine was let go."_s;
        m_control.show(shown);
        QTRY_VERIFY(problem->isVisible());
        QVERIFY(says(window->contentItem(), shown.problem));
        // In the colour of a warning, on the one line of its own the bar has for it: in the
        // bar, not over the picture, which does not move for it (the page's "focus-live-alert").
        const auto isSaidInItsLine = [&] {
            const QRectF said = problem->mapRectToScene(problem->boundingRect());
            return isInside(window, problem) && problem->property("lineCount").toInt() == 1 && said.height() >= 12 &&
                   said.bottom() <= screen->mapToScene(QPointF(0, 0)).y();
        };
        QCOMPARE(problem->property("color").value<QColor>(), QColor(u"#f59e0b"_s));
        QTRY_VERIFY(isSaidInItsLine());
        QVERIFY(!problem->property("truncated").toBool());
        // Whole, it has no tip: the pointer at rest on it brings nothing over it.
        const QObject* tip = window->findChild<QObject*>(u"controlProblemWhole"_s);
        QVERIFY(tip != nullptr);
        QTest::mouseMove(window, problem->mapToScene(QPointF(40, problem->height() / 2)).toPoint());
        QTest::qWait(1000);
        QVERIFY(!tip->property("opened").toBool());
        QTest::mouseMove(window, QPoint(5, window->height() - 5));
        letTheWallComeToRest(window);
        QCOMPARE(screen->mapToScene(QPointF(0, 0)).y(), plainTop);
        // The button is still there: they can be asked for again.
        QCOMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        QVERIFY(isInside(window, theOne(window, u"enlargedControl"_s)));
        keepPictureOf(window, u"enlarged-control-problem"_s);
        // In the narrowest window a longer one is cut at its end, and still one line: whole a
        // moment after the pointer came to rest on it, and in Details, which a click on it opens.
        window->resize(640, 400);
        shown.problem += u" Nothing that was typed meanwhile reached it: look at its screen before you type again."_s;
        m_control.show(shown);
        QTRY_VERIFY(window->width() == 640 && problem->property("truncated").toBool());
        QTRY_VERIFY(isSaidInItsLine());
        QTRY_VERIFY(isInside(window, theOne(window, u"enlargedControl"_s)));
        QTRY_VERIFY(isInside(window, theOne(window, u"enlargedClose"_s)));
        QTest::mouseMove(window, problem->mapToScene(QPointF(40, problem->height() / 2)).toPoint());
        QTRY_VERIFY(tip->property("opened").toBool());
        QCOMPARE(tip->property("text").toString(), shown.problem);
        keepPictureOf(window, u"enlarged-control-problem-narrow"_s);
        clickOn(window, problem->mapToScene(QPointF(40, problem->height() / 2)).toPoint());
        QTRY_VERIFY(theOneSeen(window, u"videoDiagnostics"_s) != nullptr);
        QVERIFY(theOneSeen(window, u"videoDiagnostics"_s)
                    ->property("videoDescription")
                    .toString()
                    .startsWith(shown.problem));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(theOneSeen(window, u"videoDiagnostics"_s) == nullptr);

        // A sentence comes from a KVM or a server, and a name from anybody: written like a page,
        // each is still letters, while they are taken and while they are not.
        const QString page =
            u"<b>bold</b> <img src=\"%1/seen.png\" width=\"40\" height=\"40\">"_s.arg(elsewhere.address());
        QList<qkvm::ui::WallTile> named = tiles;
        named[1].name = page;
        wall.machines()->setTiles(named);
        shown.problem = page;
        m_control.show(shown);
        QTRY_VERIFY(says(window->contentItem(), page));
        QTest::qWait(200);
        QVERIFY2(linesThatCouldBeAPage(window).isEmpty(), qPrintable(linesThatCouldBeAPage(window).join(u" | "_s)));
        shown.on = true;
        m_control.show(shown);
        // (The window is at its narrowest here: of the sentence the name is what stands, alone.
        // This looked for the whole sentence, which a narrow bar cut at the name's end.)
        QTRY_VERIFY(seenNamed(window, u"enlargedName"_s) == 1 && says(theOne(window, u"enlargedName"_s), page));
        QTest::qWait(200);
        QVERIFY2(linesThatCouldBeAPage(window).isEmpty(), qPrintable(linesThatCouldBeAPage(window).join(u" | "_s)));
        QCOMPARE(elsewhere.asked.size(), 0);
        // While they are taken nothing is complained of; and it is gone when the view has none.
        QCOMPARE(seenNamed(window, u"controlProblem"_s), 0);
        m_control.show({});
        QTRY_COMPARE(seenNamed(window, u"controlGoesTo"_s), 0);
        QCOMPARE(seenNamed(window, u"controlProblem"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatThePointerDoesOverThePictureGoesToTheMachineWhileItIsControlled_data()
    {
        QTest::addColumn<QSize>("pictureSize");
        QTest::newRow("bars above and below") << QSize(1920, 800);
        QTest::newRow("bars beside the picture") << QSize(1440, 1080);
    }

    void whatThePointerDoesOverThePictureGoesToTheMachineWhileItIsControlled()
    {
        QFETCH(QSize, pictureSize);
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
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOne(window, u"enlargedScreen"_s) != nullptr);
        const QQuickItem* screen = theOne(window, u"enlargedScreen"_s);
        const QQuickItem* area = theOne(window, u"controlPointer"_s);
        QVERIFY(area != nullptr);
        const QSignalSpy moved(&m_control, &qkvm::ui::ControlView::pointerMoved);
        const QSignalSpy pressed(&m_control, &qkvm::ui::ControlView::buttonChanged);
        const QSignalSpy turned(&m_control, &qkvm::ui::ControlView::wheelTurned);
        QTRY_COMPARE(screen->width(), 1280.0);
        const auto at = [&](qreal x, qreal y) { return screen->mapToScene(QPointF(x, y)).toPoint(); };
        const auto turnTheWheel = [&](QPoint where, QPoint angle) {
            QWheelEvent turn(QPointF(where), QPointF(window->mapToGlobal(where)), QPoint(), angle, Qt::NoButton,
                             Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &turn);
        };
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.picture = pictureSize;
        m_control.show(shown);

        // Not taken: what the pointer does over the picture is nothing to the machine. (The area
        // has a switch of its own for taking the pointer, beside the one every item has.)
        const auto takesThePointer = [&] { return area->property("enabled").toBool(); };
        QVERIFY(!takesThePointer());
        QTest::mouseMove(window, at(640, 376));
        QTest::mouseMove(window, at(650, 380));
        QTest::mouseClick(window, Qt::LeftButton, {}, at(650, 380));
        turnTheWheel(at(650, 380), {0, 120});
        QTest::qWait(100);
        QCOMPARE(moved.count(), 0);
        QCOMPARE(pressed.count(), 0);
        QCOMPARE(turned.count(), 0);

        // Taken: where the pointer is in the picture, each way 0 to 1 over it.
        shown.on = true;
        m_control.show(shown);
        QTRY_VERIFY(takesThePointer());
        // The Type text row remains available during control. Fit the known frame to
        // the actual viewport; toolbar changes must not change the pointer's coordinates.
        QTRY_COMPARE(seenNamed(window, u"controlTypeText"_s), 1);
        QTRY_VERIFY(screen->height() < window->height() - 48);
        const auto* statusBar = theOne(window, u"consoleStatus"_s);
        QVERIFY(statusBar != nullptr);
        QTRY_COMPARE(screen->height(), statusBar->mapToScene(QPointF()).y() - screen->mapToScene(QPointF()).y());
        QSizeF fitted(pictureSize);
        fitted.scale(QSizeF(screen->width(), screen->height()), Qt::KeepAspectRatio);
        const QRectF frame(QPointF((screen->width() - fitted.width()) / 2, (screen->height() - fitted.height()) / 2),
                           fitted);
        const auto over = [&](qreal x, qreal y) {
            return at(frame.left() + x * frame.width(), frame.top() + y * frame.height());
        };
        const auto place = [&] { return QPointF(moved.last().at(0).toDouble(), moved.last().at(1).toDouble()); };
        const auto isAbout = [](QPointF one, QPointF other) {
            return qAbs(one.x() - other.x()) < 0.003 && qAbs(one.y() - other.y()) < 0.003;
        };
        QTest::mouseMove(window, over(0.49, 0.49));
        QTest::mouseMove(window, over(0.5, 0.5));
        QTRY_VERIFY(moved.count() >= 1);
        QVERIFY2(isAbout(place(), {0.5, 0.5}), qPrintable(u"%1, %2"_s.arg(place().x()).arg(place().y())));
        QTest::mouseMove(window, over(0.25, 0.75));
        QTRY_VERIFY2(isAbout(place(), {0.25, 0.75}), qPrintable(u"%1, %2"_s.arg(place().x()).arg(place().y())));
        // Over a letterbox bar it is outside the picture, on the correct axis.
        const bool above = frame.top() > 1;
        QTest::mouseMove(window,
                         above ? at(screen->width() / 2, frame.top() / 2) : at(frame.left() / 2, screen->height() / 2));
        QTRY_VERIFY2(above ? place().y() < 0 : place().x() < 0,
                     qPrintable(u"%1, %2"_s.arg(place().x()).arg(place().y())));
        // And out of the picture's tile altogether, over the window's bar: beside it as well.
        QTest::mouseMove(window, over(0.25, 0.25));
        QTRY_VERIFY(place().y() > 0);
        QTest::mouseMove(window, at(320, -20));
        QTRY_VERIFY2(place().x() < 0 || place().y() < 0, qPrintable(u"%1, %2"_s.arg(place().x()).arg(place().y())));

        // The buttons, each by its number, down and up, where the pointer is.
        const auto last = [&] { return QPair<int, bool>(pressed.last().at(0).toInt(), pressed.last().at(1).toBool()); };
        int presses = 0;
        const QList<QPair<Qt::MouseButton, int>> buttons = {{Qt::LeftButton, 1},
                                                            {Qt::RightButton, 2},
                                                            {Qt::MiddleButton, 3},
                                                            {Qt::BackButton, 4},
                                                            {Qt::ForwardButton, 5}};
        for (const auto& [button, number] : buttons) {
            QTest::mousePress(window, button, {}, over(0.75, 0.25));
            ++presses;
            QTRY_COMPARE(pressed.count(), presses);
            QCOMPARE(last(), (QPair<int, bool>(number, true)));
            QVERIFY2(isAbout(place(), {0.75, 0.25}), qPrintable(u"%1, %2"_s.arg(place().x()).arg(place().y())));
            QTest::mouseRelease(window, button, {}, over(0.75, 0.25));
            ++presses;
            QTRY_COMPARE(pressed.count(), presses);
            QCOMPARE(last(), (QPair<int, bool>(number, false)));
        }
        // Clicked twice quickly, it is pressed twice: the machine makes its own double click.
        QTest::mouseDClick(window, Qt::LeftButton, {}, over(0.5, 0.5));
        QTRY_COMPARE(pressed.count(), presses + 4);
        for (int one = 0; one < 4; ++one) {
            QCOMPARE(pressed.at(presses + one).at(0).toInt(), 1);
            QCOMPARE(pressed.at(presses + one).at(1).toBool(), one % 2 == 0);
        }
        // Dragged: the places on the way, and the end where the button is let go.
        QTest::mousePress(window, Qt::LeftButton, {}, over(0.25, 0.25));
        QTest::mouseMove(window, over(0.5, 0.5));
        QTest::mouseMove(window, over(0.75, 0.75));
        QTest::mouseRelease(window, Qt::LeftButton, {}, over(0.75, 0.75));
        QTRY_COMPARE(pressed.count(), presses + 6);
        QVERIFY2(isAbout(place(), {0.75, 0.75}), qPrintable(u"%1, %2"_s.arg(place().x()).arg(place().y())));

        // The wheel, as Qt says a turn: away from the hand, towards it, and sideways.
        turnTheWheel(over(0.5, 0.5), {0, 120});
        QTRY_COMPARE(turned.count(), 1);
        QCOMPARE(turned.last().at(0).toInt(), 0);
        QCOMPARE(turned.last().at(1).toInt(), 120);
        turnTheWheel(over(0.5, 0.5), {0, -240});
        turnTheWheel(over(0.5, 0.5), {120, 0});
        QTRY_COMPARE(turned.count(), 3);
        QCOMPARE(turned.at(1).at(1).toInt(), -240);
        QCOMPARE(turned.at(2).at(0).toInt(), 120);
        keepPictureOf(window, u"enlarged-controlled-pointer"_s);
        // The new status strip lies outside remote input, including wheel and clicks.
        const auto beforeStatusClick = pressed.count();
        const QPoint inStatus = statusBar->mapToScene(QPointF(20, 12)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, {}, inStatus);
        turnTheWheel(inStatus, {0, 120});
        QCOMPARE(pressed.count(), beforeStatusClick);
        QCOMPARE(turned.count(), 3);

        // Given back: nothing more of it.
        shown.on = false;
        m_control.show(shown);
        QTRY_VERIFY(!takesThePointer());
        const qsizetype movedBefore = moved.count();
        const qsizetype pressedBefore = pressed.count();
        QTest::mouseMove(window, at(100, 100));
        QTest::mouseMove(window, at(200, 200));
        QTest::mouseClick(window, Qt::LeftButton, {}, at(200, 200));
        turnTheWheel(at(200, 200), {0, 120});
        QTest::qWait(100);
        // (The pointer leaving the picture's place may still be said, as what it is: beside it.)
        for (qsizetype later = movedBefore; later < moved.count(); ++later) {
            QVERIFY(moved.at(later).at(0).toDouble() < 0 && moved.at(later).at(1).toDouble() < 0);
        }
        QCOMPARE(pressed.count(), pressedBefore);
        QCOMPARE(turned.count(), 3);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // b10: a machine shown large is shown fitted or at a scale of its own pixels, chosen under the
    // picture as on the page; where it is larger than the window it scrolls, and is moved by a drag or
    // the wheel only while it is watched; the pointer is exact over it at every place it is scrolled
    // to; and nothing in the bar moves for the choice, whether or not the machine has a picture.
    void aMachineShownLargeIsShownAtTheSizeChosenAndThePointerStaysExact()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        tiles[0].pictureSize = QSize(1920, 1080);
        tiles[1].stream = 0;
        tiles[1].title = u"No signal"_s;
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        QVERIFY(bringUp(window));
        wall.enlarge(tiles[0].key);
        QTRY_VERIFY(theOneSeen(window, u"enlargedScale"_s) != nullptr);
        auto* view = window->findChild<QQuickItem*>(u"enlargedView"_s);
        const QQuickItem* screen = theOne(window, u"enlargedScreen"_s);
        const QQuickItem* pick = theOne(window, u"enlargedScale"_s);
        const QQuickItem* details = theOne(window, u"enlargedVideoDetails"_s);
        const QQuickItem* viewing = theOne(window, u"controlViewing"_s);
        QVERIFY(view != nullptr && screen != nullptr && pick != nullptr && details != nullptr && viewing != nullptr);
        const auto placeOf = [](const QQuickItem* item) { return item->mapToScene(QPointF(0, 0)); };
        const QPointF pickAt = placeOf(pick);
        const QPointF detailsAt = placeOf(details);
        const QPointF viewingAt = placeOf(viewing);
        QCOMPARE(chosenIn(window, u"enlargedScale"_s), u"Fit"_s);
        QTRY_COMPARE(screen->width(), view->width());
        QCOMPARE(seenNamed(window, u"enlargedScrollX"_s), 0);
        const QSignalSpy chosen(&wall, &qkvm::ui::WallView::scaleChosen);

        // 200 %: the machine's 1920 by 1080, twice, in the window's own pixels (one to each here).
        QVERIFY(chooseIn(window, u"enlargedScale"_s, u"200 %"_s));
        QCOMPARE(wall.enlargedScale(), u"200"_s);
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.last().at(0).toString(), tiles[0].key);
        QCOMPARE(chosen.last().at(1).toString(), u"200"_s);
        QTRY_COMPARE(screen->width(), 3840.0);
        QCOMPARE(screen->height(), 2160.0);
        QTRY_COMPARE(seenNamed(window, u"enlargedScrollX"_s), 1);
        QCOMPARE(seenNamed(window, u"enlargedScrollY"_s), 1);
        // Nothing of the bar moved.
        QCOMPARE(placeOf(pick), pickAt);
        QCOMPARE(placeOf(details), detailsAt);
        QCOMPARE(placeOf(viewing), viewingAt);

        // Watched: a drag moves it, and so does the wheel.
        const auto inView = [&](qreal x, qreal y) { return view->mapToScene(QPointF(x, y)).toPoint(); };
        const qreal leftBefore = view->property("contentX").toReal();
        const qreal topBefore = view->property("contentY").toReal();
        QTest::mousePress(window, Qt::LeftButton, {}, inView(600, 400));
        for (int step = 1; step <= 10; ++step) {
            QTest::mouseMove(window, inView(600 - 20 * step, 400 - 10 * step), 20);
        }
        QTest::mouseRelease(window, Qt::LeftButton, {}, inView(400, 300), 20);
        QTRY_VERIFY(view->property("contentX").toReal() > leftBefore + 100);
        QVERIFY(view->property("contentY").toReal() > topBefore + 50);
        const auto turnTheWheel = [&](QPoint where, QPoint angle) {
            QWheelEvent turn(QPointF(where), QPointF(window->mapToGlobal(where)), QPoint(), angle, Qt::NoButton,
                             Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window, &turn);
        };
        const qreal topThen = view->property("contentY").toReal();
        turnTheWheel(inView(500, 300), {0, -120});
        QTRY_VERIFY(view->property("contentY").toReal() > topThen);

        // Controlled: the pointer is where it is in the machine's picture, wherever it is scrolled to;
        // a drag and the wheel are the machine's, and move nothing here.
        const QSignalSpy moved(&m_control, &qkvm::ui::ControlView::pointerMoved);
        const QSignalSpy turned(&m_control, &qkvm::ui::ControlView::wheelTurned);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        shown.picture = QSize(1920, 1080);
        m_control.show(shown);
        QTRY_VERIFY(theOne(window, u"controlPointer"_s)->property("enabled").toBool());
        QVERIFY(view->setProperty("contentX", 700));
        QVERIFY(view->setProperty("contentY", 450));
        QTRY_COMPARE(view->property("contentX").toReal(), 700.0);
        const auto place = [&] { return QPointF(moved.last().at(0).toDouble(), moved.last().at(1).toDouble()); };
        for (const QPointF at : {QPointF(500, 300), QPointF(10, 10), QPointF(900, 500)}) {
            QTest::mouseMove(window, inView(at.x() - 1, at.y()));
            QTest::mouseMove(window, inView(at.x(), at.y()));
            const QPointF wanted((at.x() + 700) / 3840, (at.y() + 450) / 2160);
            QTRY_VERIFY2(
                qAbs(place().x() - wanted.x()) < 0.0005 && qAbs(place().y() - wanted.y()) < 0.0005,
                qPrintable(u"%1, %2 for %3, %4"_s.arg(place().x()).arg(place().y()).arg(wanted.x()).arg(wanted.y())));
        }
        QTest::mousePress(window, Qt::LeftButton, {}, inView(500, 300));
        for (int step = 1; step <= 10; ++step) {
            QTest::mouseMove(window, inView(500 - 20 * step, 300 - 10 * step), 20);
        }
        QTest::mouseRelease(window, Qt::LeftButton, {}, inView(300, 200), 20);
        turnTheWheel(inView(500, 300), {0, -120});
        QTRY_COMPARE(turned.count(), 1);
        QCOMPARE(view->property("contentX").toReal(), 700.0);
        QCOMPARE(view->property("contentY").toReal(), 450.0);
        keepPictureOf(window, u"enlarged-scaled-200"_s);
        shown.on = false;
        m_control.show(shown);

        // Fit again: as large as the window has room for, and no scroll bars.
        QVERIFY(chooseIn(window, u"enlargedScale"_s, u"Fit"_s));
        QTRY_COMPARE(screen->width(), view->width());
        QTRY_COMPARE(seenNamed(window, u"enlargedScrollX"_s), 0);
        // A machine without a picture has no choice of its size; nothing moves for that either.
        wall.stepEnlarged(1);
        QTRY_COMPARE(seenNamed(window, u"enlargedScale"_s), 0);
        QCOMPARE(placeOf(details), detailsAt);
        QCOMPARE(placeOf(viewing), viewingAt);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whileKeyboardAndMouseAreTakenTheMachinesNameStandsWhole_data()
    {
        QTest::addColumn<int>("width");
        QTest::newRow("640, as narrow as a window can be made") << 640;
        QTest::newRow("800") << 800;
        QTest::newRow("1280") << 1280;
    }

    void whileKeyboardAndMouseAreTakenTheMachinesNameStandsWhole()
    {
        QFETCH(int, width);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        auto tiles = qkvm::ui::pretendWall();
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        auto* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr && bringUp(window));
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        m_control.show(shown);

        window->resize(width, 500);
        for (const QString& title :
             {u"Build server 12, rack B4 upper"_s,
              u"The build server in the second rack of the back room, upper half, left of the door"_s}) {
            tiles[1].name = title;
            wall.machines()->setTiles(tiles);
            const auto* name = theOne(window, u"enlargedName"_s);
            QTRY_VERIFY(name != nullptr && name->isVisible() && isInside(window, name));
            QTRY_COMPARE(name->property("text").toString(), title);
            QCOMPARE(name->property("elide").toInt(), static_cast<int>(Qt::ElideMiddle));
            QTRY_VERIFY(window->title().contains(title));
            if (width >= 1280 && title.size() < 40)
                QTRY_VERIFY(!name->property("truncated").toBool());
            const auto* wayBack = theOne(window, u"controlWayBack"_s);
            const auto* button = theOne(window, u"controlGiveBack"_s);
            QTRY_VERIFY(isInside(window, wayBack) && wayBack->width() >= wayBack->implicitWidth() - 0.5);
            QVERIFY(wayBack->property("text").toString().contains(u"Right Ctrl"_s));
            QTRY_VERIFY(isInside(window, button) && button->width() >= button->implicitWidth() - 0.5);
            keepPictureOf(window, u"enlarged-controlled-name-%1-%2"_s.arg(width).arg(title.size()));
        }
        shown.on = false;
        m_control.show(shown);
        QTRY_COMPARE(window->title(), u"qKVM"_s);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whileKeyboardAndMouseAreAskedForTheBarSaysSoAndOffersToStop()
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
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s) != nullptr);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        // Not asked for: nothing of it.
        QCOMPARE(seenNamed(window, u"controlAsking"_s), 0);
        const QQuickItem* stop = theOne(window, u"enlargedControl"_s);
        QVERIFY(says(stop, u"Control"_s));

        // Asked for, and the machine has not answered: the bar says so, and offers to stop asking.
        shown.asking = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlAsking"_s), 1);
        // (Once the bar is laid out: the sentence is chosen by the room there is for it.)
        QTRY_VERIFY(says(window->contentItem(), u"Asking..."_s));
        // (Control says it, itself, as the page's does; a click on it again stops it.)
        QCOMPARE(theOne(window, u"enlargedControl"_s), stop);
        QVERIFY(says(stop, u"Asking..."_s));
        QCOMPARE(stop->property("hint").toString(), u"Stop asking NAS for its keyboard and mouse"_s);
        QTRY_VERIFY(isInside(window, stop) && isInside(window, theOne(window, u"controlAsking"_s)));
        keepPictureOf(window, u"enlarged-control-asking"_s);
        const QSignalSpy giveBackAsked(&m_control, &qkvm::ui::ControlView::giveBackAsked);
        clickOn(window, stop);
        QTRY_COMPARE(giveBackAsked.count(), 1);

        // Whoever goes on to something else in the window meanwhile is said to have: a key
        // pressed here, and a box opened. What is done to the picture is not that.
        const QSignalSpy wentOn(&m_control, &qkvm::ui::ControlView::wentOnSaid);
        clickOn(window, theOne(window, u"enlargedScreen"_s));
        QTest::qWait(100);
        QCOMPARE(wentOn.count(), 0);
        QTest::keyClick(window, Qt::Key_A);
        QTRY_VERIFY(wentOn.count() >= 1);
        // (Said as what it was, a key: the bar then says that, and not that the machine was late.)
        QVERIFY(wentOn.constLast().at(0).toBool());
        const qsizetype afterTheKey = wentOn.count();
        const QObject* typeText = window->findChild<QObject*>(u"typeText"_s);
        QVERIFY(typeText != nullptr);
        actions.showForm({{u"kind"_s, u"text"_s}, {u"machine"_s, tiles[1].name}, {u"problem"_s, QString()}});
        QTRY_VERIFY(typeText->property("opened").toBool());
        QVERIFY(wentOn.count() > afterTheKey);
        QVERIFY(!wentOn.constLast().at(0).toBool());
        actions.showForm({});
        QTRY_VERIFY(!typeText->property("opened").toBool());

        // In the narrowest window the way to stop asking is still in sight.
        window->resize(640, 400);
        QTRY_VERIFY(window->width() == 640 && isInside(window, stop));
        QTRY_VERIFY(isInside(window, theOne(window, u"enlargedClose"_s)));
        keepPictureOf(window, u"enlarged-control-asking-narrow"_s);

        // Answered, one way or the other: gone.
        shown.asking = false;
        m_control.show(shown);
        QTRY_VERIFY(says(stop, u"Control"_s));
        QCOMPARE(seenNamed(window, u"controlAsking"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBarNamesTheLeaveKeyAndTheButtonBesideItAlways_data() { aBarOfEveryWidthWithEveryKindOfName(); }

    void theBarNamesTheLeaveKeyAndTheButtonBesideItAlways()
    {
        // (Turned round: "on a keyboard without the leave-key the bar names the button alone".
        // Whether the keyboard that is plugged in has the key cannot be asked of Windows, which
        // knows the layout's keys: the bar was told that it has one on every usual keyboard, a
        // small one without a right Ctrl too. So it names both ways back, in every width.)
        QFETCH(int, width);
        QFETCH(QString, name);
        QFETCH(QString, kind);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        tiles[1].name = name;
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(width, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGoesTo"_s), 1);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        const QQuickItem* goesTo = theOne(window, u"controlGoesTo"_s);
        const QQuickItem* giveBack = theOne(window, u"controlGiveBack"_s);
        const auto isWhole = [&](const QQuickItem* line) {
            return isInside(window, line) && line->width() >= line->implicitWidth() - 0.5 &&
                   !line->property("truncated").toBool();
        };
        // (Kept as it is drawn, whatever is found of it below.)
        QTest::qWait(150);
        keepPictureOf(window, u"bar-controlled-%1-%2"_s.arg(width).arg(kind));
        // The key by its name, whole; and the button, whole, to its right.
        QTRY_COMPARE(seenNamed(window, u"controlWayBack"_s), 1);
        const QQuickItem* wayBack = theOne(window, u"controlWayBack"_s);
        QTRY_VERIFY(isWhole(wayBack) && isWhole(giveBack));
        QVERIFY2(wayBack->property("text").toString().contains(u"Right Ctrl"_s),
                 qPrintable(wayBack->property("text").toString()));
        QVERIFY(says(giveBack, u"Control"_s));
        // A short name leaves room for all of it in every width: the whole sentence, and the
        // way back in full.
        if (kind == u"short"_s) {
            QVERIFY(says(theOne(window, u"enlargedName"_s), u"NAS"_s));
            QVERIFY(theOne(window, u"controlWayBack"_s)->property("text").toString().contains(u"Right Ctrl"_s));
        }
        // Which machine is always in sight.
        QVERIFY(isInside(window, goesTo));
        QCOMPARE(theOne(window, u"enlargedName"_s)->property("text").toString(), name);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whileKeyboardAndMouseAreAskedForTheWordForItStandsWhole_data() { aBarOfEveryWidthWithEveryKindOfName(); }

    void whileKeyboardAndMouseAreAskedForTheWordForItStandsWhole()
    {
        QFETCH(int, width);
        QFETCH(QString, name);
        QFETCH(QString, kind);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        tiles[1].name = name;
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(width, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.asking = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlAsking"_s), 1);
        const QQuickItem* asking = theOne(window, u"controlAsking"_s);
        const QQuickItem* stop = theOne(window, u"enlargedControl"_s);
        QTRY_VERIFY(says(stop, u"Asking..."_s));
        const QQuickItem* machine = theOne(window, u"enlargedName"_s);
        QVERIFY(machine != nullptr);
        const auto isWhole = [&](const QQuickItem* line) {
            return isInside(window, line) && line->width() >= line->implicitWidth() - 0.5 &&
                   !line->property("truncated").toBool();
        };
        // (Kept as it is drawn, whatever is found of it below.)
        QTest::qWait(150);
        keepPictureOf(window, u"bar-asking-%1-%2"_s.arg(width).arg(kind));
        // Admission stays whole in the status strip; the machine stays named in the header.
        const auto said = [&] { return asking->property("text").toString(); };
        QTRY_VERIFY2(isWhole(asking) && said() == u"Asking..."_s, qPrintable(said()));
        QCOMPARE(machine->property("text").toString(), name);
        // What gives way is the machine's name beside it, which is cut at its end where it has
        // to be, and never to nothing. The way to stop asking and the bar's other buttons keep
        // their size and their place, in the order they are read in.
        QTRY_VERIFY(isWhole(stop));
        QVERIFY(isSeen(machine) && isInside(window, machine));
        QVERIFY2(machine->width() >= qMin(40.0, machine->implicitWidth() - 0.5),
                 qPrintable(QString::number(machine->width())));
        QVERIFY(isInside(window, asking));
        QVERIFY(theBarsButtonsKeepTheirPlace(window));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theBarOfAMachineShownLargeKeepsItsButtonsInTheWindowWhateverTheName_data()
    {
        aBarOfEveryWidthWithEveryKindOfName();
    }

    void theBarOfAMachineShownLargeKeepsItsButtonsInTheWindowWhateverTheName()
    {
        // The plain bar, with the button that takes keyboard and mouse in it: with a name of
        // thirty letters the narrowest window had no room for all of it, and the buttons at its
        // end stood outside the window. The name is what gives way, cut at its end.
        QFETCH(int, width);
        QFETCH(QString, name);
        QFETCH(QString, kind);
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        tiles[1].name = name;
        wall.machines()->setTiles(tiles);
        qkvm::ui::SessionView session;
        session.show(signedIn());
        qkvm::ui::ActionsView actions;
        actions.setAllowed(true);
        QQmlApplicationEngine engine;
        QQuickWindow* window = load(engine, about, session, wall, &actions);
        QVERIFY(window != nullptr);
        window->resize(width, 500);
        QVERIFY(bringUp(window));
        QTRY_COMPARE(window->width(), width);
        wall.enlarge(tiles[1].key);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedControl"_s), 1);
        QTest::qWait(150);
        keepPictureOf(window, u"bar-plain-%1-%2"_s.arg(width).arg(kind));
        QTRY_VERIFY(theBarsButtonsKeepTheirPlace(window));
        const QQuickItem* machine = theOne(window, u"enlargedName"_s);
        QVERIFY(machine != nullptr && isSeen(machine) && isInside(window, machine));
        QVERIFY2(machine->width() >= qMin(40.0, machine->implicitWidth() - 0.5),
                 qPrintable(QString::number(machine->width())));
        // A name that has room is whole.
        if (kind == u"short"_s || width >= 1280) {
            QVERIFY(!machine->property("truncated").toBool() || kind == u"long"_s);
        }
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void whatIsToBeKnownWhileKeyboardAndMouseAreTakenIsSaidInTheBar()
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
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s) != nullptr);
        // Taken, with nothing to be known: the bar is as it was, and has no such line.
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QCOMPARE(seenNamed(window, u"controlNote"_s), 0);
        const QQuickItem* screen = theOne(window, u"enlargedScreen"_s);
        letTheWallComeToRest(window);
        const qreal plainTop = screen->mapToScene(QPointF(0, 0)).y();

        // Something is: a key that was held from before keeps what is typed from the machine.
        // Said on the line of its own the bar has for it, in the bar and not over the picture,
        // in the colour of a warning; and the way back stays as it was.
        shown.note = u"Held when keyboard and mouse were last given back, and not heard to be let go since: Left Ctrl. "
                     "Until each is let go or pressed anew, no other key and no click goes to the machine."_s;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlNote"_s), 1);
        const QQuickItem* note = theOne(window, u"controlNote"_s);
        QVERIFY(says(window->contentItem(), shown.note));
        const auto isSaidInItsLine = [&] {
            const QRectF said = note->mapRectToScene(note->boundingRect());
            return isInside(window, note) && note->property("lineCount").toInt() == 1 && said.height() >= 12 &&
                   said.bottom() <= screen->mapToScene(QPointF(0, 0)).y();
        };
        QTRY_VERIFY(isSaidInItsLine());
        QCOMPARE(note->property("color").value<QColor>(), QColor(u"#f59e0b"_s));
        // (On the line the bar has for it always: the picture does not move for it.)
        letTheWallComeToRest(window);
        QCOMPARE(screen->mapToScene(QPointF(0, 0)).y(), plainTop);
        QCOMPARE(seenNamed(window, u"controlGiveBack"_s), 1);
        QCOMPARE(seenNamed(window, u"controlWayBack"_s), 1);
        QVERIFY(isInside(window, theOne(window, u"controlGiveBack"_s)));
        QCOMPARE(seenNamed(window, u"controlProblem"_s), 0);
        keepPictureOf(window, u"enlarged-controlled-note"_s);
        // In the narrowest window it is cut at its end, and still one line: whole in Details.
        window->resize(640, 400);
        QTRY_VERIFY(window->width() == 640 && note->property("truncated").toBool());
        QTRY_VERIFY(isSaidInItsLine());
        QTRY_VERIFY(isInside(window, theOne(window, u"controlGiveBack"_s)));
        keepPictureOf(window, u"enlarged-controlled-note-narrow"_s);
        // Whole a moment after the pointer came to rest on it, and first in Details, which a click on
        // it opens.
        QTest::mouseMove(window, note->mapToScene(QPointF(40, note->height() / 2)).toPoint());
        const QObject* tip = window->findChild<QObject*>(u"controlNoteWhole"_s);
        QVERIFY(tip != nullptr);
        QTRY_VERIFY(tip->property("opened").toBool());
        QCOMPARE(tip->property("text").toString(), shown.note);
        clickOn(window, note->mapToScene(QPointF(40, note->height() / 2)).toPoint());
        QTRY_VERIFY(theOneSeen(window, u"videoDiagnostics"_s) != nullptr);
        QVERIFY(
            theOneSeen(window, u"videoDiagnostics"_s)->property("videoDescription").toString().startsWith(shown.note));
        clickOn(window, theOne(window, u"enlargedVideoDetails"_s));
        QTRY_VERIFY(theOneSeen(window, u"videoDiagnostics"_s) == nullptr);
        // It is letters, whoever wrote it.
        QVERIFY2(linesThatCouldBeAPage(window).isEmpty(), qPrintable(linesThatCouldBeAPage(window).join(u" | "_s)));

        // Nothing more to be known: the line is gone, and the picture is where it was. And it is
        // said only while they are taken.
        shown.note.clear();
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlNote"_s), 0);
        shown.note = u"Something."_s;
        shown.on = false;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"controlGiveBack"_s), 0);
        QCOMPARE(seenNamed(window, u"controlNote"_s), 0);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    void theButtonThatAsksDoesNothingWhileAnEarlierAskingIsStillUnderWay()
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
        wall.enlarge(tiles[1].key);
        QTRY_VERIFY(theOne(window, u"enlargedControl"_s) != nullptr);
        const QQuickItem* button = theOne(window, u"enlargedControl"_s);
        // An asking was given up, and why is said; the machine has not answered it yet. (As it
        // is said while the machine owes a word: not "Click Control again", which cannot work
        // then.)
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.busy = true;
        shown.problem = u"A key was pressed before the machine answered, so keyboard and mouse were not taken. "
                        "Control can be clicked again once the machine has answered, or after the time it is given "
                        "for that."_s;
        m_control.show(shown);
        QTRY_VERIFY(button->isVisible());
        QVERIFY(says(window->contentItem(), shown.problem));
        // The button is there, and says by its look that it does nothing just now: it asks
        // nobody, by the mouse or by the keyboard. (It keeps the keys where it has them: a button
        // that cannot be pressed would lose them, and the console's keys with them.)
        const QSignalSpy takeAsked(&m_control, &qkvm::ui::ControlView::takeAsked);
        QTRY_VERIFY(button->property("resting").toBool());
        QCOMPARE(button->opacity(), 0.5);
        clickOn(window, button);
        QTest::qWait(100);
        QCOMPARE(takeAsked.count(), 0);
        // The answer has come: it asks again.
        shown.busy = false;
        m_control.show(shown);
        QTRY_VERIFY(!button->property("resting").toBool());
        QCOMPARE(button->opacity(), 1.0);
        clickOn(window, button);
        QTRY_COMPARE(takeAsked.count(), 1);
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runScreensTests<TestConsoleScreens>(argc, argv);
}

#include "test_console_screens.moc"
