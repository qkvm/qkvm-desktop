#include <cmath>

#include "screens_test.h"

// A machine shown large at a scale of its own pixels (b10), as a person sees it and points at it: the
// picture laid out by the pictures that come, and every pixel of the machine's screen within reach of
// the pointer, wherever it is scrolled to. (A program of its own: the console's is long enough.)
class TestScaledScreens : public ScreensTest {
    Q_OBJECT

    // Where the pointer is in the machine's picture, as the screen says it, once it is at this place of
    // the view (moved there from beside it, so that it moves); and where in the view that is, on the
    // window's whole pixel nearest to it, which is where a mouse is.
    static QPointF placeAt(QQuickWindow* window, const QQuickItem* view, const QSignalSpy& moved, QPointF at,
                           QPointF& onAPixel)
    {
        const QPoint there = view->mapToScene(at).toPoint();
        onAPixel = view->mapFromScene(QPointF(there));
        QTest::mouseMove(window, there - QPoint(1, 0));
        QTest::mouseMove(window, there);
        if (moved.isEmpty()) {
            return {-2, -2};
        }
        return {moved.last().at(0).toDouble(), moved.last().at(1).toDouble()};
    }

    // Whether the place said is the one wanted, to a hundredth of one of the machine's pixels.
    static bool exact(QPointF said, QPointF wanted, QSize frame)
    {
        return qAbs(said.x() - wanted.x()) * frame.width() < 0.01 &&
               qAbs(said.y() - wanted.y()) * frame.height() < 0.01;
    }

    static QString both(QPointF said, QPointF wanted)
    {
        return u"%1, %2 for %3, %4"_s.arg(said.x()).arg(said.y()).arg(wanted.x()).arg(wanted.y());
    }

private slots:
    // After each test, when its windows are gone: what they were handed goes too.
    void cleanup() { forgetWhatTheWindowsWereHanded(); }

    // The picture the engine draws is the one that came: at a scale it is laid out by that, and the
    // pointer is mapped by it, never by the size the machine's status said last. A machine started into
    // its firmware's setup (1024 by 768, as its status says) whose system then shows 1920 by 1080: until
    // the next status, a click would land 12 % of the height off. And when the frames change their size,
    // the picture and the pointer follow at once.
    void atAScaleThePictureAndThePointerGoByThePicturesThatCome()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        tiles[0].pictureSize = QSize(1024, 768);
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
        wall.setLive(true);
        wall.setLiveStream(9001);
        QTRY_VERIFY(theOneSeen(window, u"enlargedScale"_s) != nullptr);
        auto* view = window->findChild<QQuickItem*>(u"enlargedView"_s);
        const QQuickItem* screen = theOne(window, u"enlargedScreen"_s);
        QVERIFY(view != nullptr && screen != nullptr);
        QVERIFY(chooseIn(window, u"enlargedScale"_s, u"100 %"_s));
        const QSignalSpy moved(&m_control, &qkvm::ui::ControlView::pointerMoved);

        // Its live pictures come at 1920 by 1080: one of this window's pixels to each of theirs.
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        shown.picture = QSize(1920, 1080);
        m_control.show(shown);
        QTRY_VERIFY(theOne(window, u"controlPointer"_s)->property("enabled").toBool());
        QTRY_COMPARE(screen->size(), QSizeF(1920, 1080));
        QVERIFY(view->setProperty("contentX", 300));
        QVERIFY(view->setProperty("contentY", 200));
        QTRY_COMPARE(view->property("contentY").toReal(), 200.0);
        QSize frame(1920, 1080);
        QPointF at;
        for (const QPointF place : {QPointF(10, 10), QPointF(400, 300), QPointF(900, 420)}) {
            const QPointF said = placeAt(window, view, moved, place, at);
            const QPointF wanted((at.x() + 300) / frame.width(), (at.y() + 200) / frame.height());
            QVERIFY2(exact(said, wanted, frame), qPrintable(both(said, wanted)));
        }

        // The frames are smaller now (640 by 480): the picture is that large at once, in the middle of
        // the view, and the pointer is mapped by it.
        frame = QSize(640, 480);
        shown.picture = frame;
        m_control.show(shown);
        QCOMPARE(screen->size(), QSizeF(frame));
        QTRY_COMPARE(view->property("contentX").toReal(), 0.0);
        QTRY_COMPARE(view->property("contentY").toReal(), 0.0);
        const QPointF corner((view->width() - frame.width()) / 2, (view->height() - frame.height()) / 2);
        QCOMPARE(screen->mapToItem(view, QPointF(0, 0)), corner);
        for (const QPointF in : {QPointF(1, 1), QPointF(100, 50), QPointF(639, 479)}) {
            const QPointF said = placeAt(window, view, moved, corner + in, at);
            const QPointF wanted((at.x() - corner.x()) / frame.width(), (at.y() - corner.y()) / frame.height());
            QVERIFY2(exact(said, wanted, frame), qPrintable(both(said, wanted)));
        }

        // A live whose size only its decoder finds says none with its pictures: the size they were
        // decoded at, as the live's details say it.
        shown.picture = QSize();
        m_control.show(shown);
        wall.setLiveDiagnostics({{u"width"_s, 1600}, {u"height"_s, 900}});
        QTRY_COMPARE(screen->size(), QSizeF(1600, 900));
        // And without a live picture, its stills: by what its card says.
        shown.on = false;
        m_control.show(shown);
        wall.setLiveStream(0);
        QTRY_COMPARE(screen->size(), QSizeF(1024, 768));
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }

    // The scroll bars have a room of their own beside the picture, and never lie over what the pointer
    // does there: every pixel of the machine's screen can be pointed at and clicked, its last on the
    // right and at the bottom as well, wherever it is scrolled to (a click on the machine's taskbar or
    // on a scroll bar at its right edge went to the view's bar, which jumped). The room is kept while
    // the picture is at a scale, bars or none: they do not come and go under the pointer, which keeps
    // its size and place when the pictures grow larger than the view.
    void theWholeScreenIsWithinThePointersReachAndTheBarsNeverLieOverIt()
    {
        qkvm::ui::AboutModel about;
        qkvm::ui::WallView wall;
        QList<qkvm::ui::WallTile> tiles = qkvm::ui::pretendWall();
        tiles[0].pictureSize = QSize(1920, 1080);
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
        wall.setLive(true);
        wall.setLiveStream(9001);
        QTRY_VERIFY(theOneSeen(window, u"enlargedScale"_s) != nullptr);
        auto* view = window->findChild<QQuickItem*>(u"enlargedView"_s);
        const QQuickItem* pointer = theOne(window, u"controlPointer"_s);
        QVERIFY(view != nullptr && pointer != nullptr);
        QVERIFY(chooseIn(window, u"enlargedScale"_s, u"100 %"_s));
        const QSignalSpy moved(&m_control, &qkvm::ui::ControlView::pointerMoved);
        const QSignalSpy pressed(&m_control, &qkvm::ui::ControlView::buttonChanged);
        qkvm::ui::ControlView::Shown shown;
        shown.possible = true;
        shown.on = true;
        const QSize frame(1920, 1080);
        shown.picture = frame;
        m_control.show(shown);
        QTRY_VERIFY(pointer->property("enabled").toBool());
        QTRY_COMPARE(seenNamed(window, u"enlargedScrollX"_s), 1);
        QCOMPARE(seenNamed(window, u"enlargedScrollY"_s), 1);
        const auto inScene = [](const QQuickItem* item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const QRectF area = inScene(pointer);
        QCOMPARE(area, inScene(view));
        // Neither bar lies over the pointer's area, nor over the bar under it: each in its room at its edge, in the
        // window, and as large as a bar is.
        const QRectF status = inScene(theOne(window, u"consoleStatus"_s));
        for (const QString& name : {u"enlargedScrollX"_s, u"enlargedScrollY"_s}) {
            const QQuickItem* item = theOne(window, name);
            const QRectF bar = inScene(item);
            QVERIFY2(!bar.intersects(area), qPrintable(name));
            QVERIFY2(!bar.intersects(status), qPrintable(name));
            QVERIFY2(isInside(window, item), qPrintable(name));
            QVERIFY2(bar.width() >= 8 && bar.height() >= 8, qPrintable(name));
        }

        // Scrolled as far as it goes, right and down: the view's last pixel is the machine's last, and a
        // click there is the machine's, and moves nothing here.
        const qreal farRight = view->property("contentWidth").toReal() - view->width();
        const qreal farDown = view->property("contentHeight").toReal() - view->height();
        QVERIFY(farRight > 100 && farDown > 100);
        QVERIFY(view->setProperty("contentX", farRight));
        QVERIFY(view->setProperty("contentY", farDown));
        QTRY_COMPARE(view->property("contentY").toReal(), farDown);
        const auto lastOf = [](qreal length) { return std::ceil(length) - 1; };
        const QPointF last(lastOf(view->width()), lastOf(view->height()));
        for (const QPointF place : {last, QPointF(3, last.y()), QPointF(last.x(), 3)}) {
            QPointF at;
            const QPointF said = placeAt(window, view, moved, place, at);
            const QPointF pixel(std::floor(said.x() * frame.width()), std::floor(said.y() * frame.height()));
            const QPointF wanted(std::floor(at.x() + farRight), std::floor(at.y() + farDown));
            QVERIFY2(pixel == wanted, qPrintable(both(pixel, wanted)));
            const qsizetype before = pressed.size();
            const QPoint there = view->mapToScene(at).toPoint();
            QTest::mousePress(window, Qt::LeftButton, {}, there);
            QTest::mouseRelease(window, Qt::LeftButton, {}, there);
            QTRY_COMPARE(pressed.size(), before + 2);
            QCOMPARE(pressed.at(before), (QVariantList{1, true}));
            QCOMPARE(pressed.at(before + 1), (QVariantList{1, false}));
            QCOMPARE(view->property("contentX").toReal(), farRight);
            QCOMPARE(view->property("contentY").toReal(), farDown);
        }
        keepPictureOf(window, u"enlarged-scaled-far-corner"_s);
        // The machine's last pixel on the right, and at the bottom, is that of the view's last.
        {
            QPointF at;
            const QPointF said = placeAt(window, view, moved, last, at);
            QCOMPARE(std::floor(said.x() * frame.width()), frame.width() - 1.0);
            QCOMPARE(std::floor(said.y() * frame.height()), frame.height() - 1.0);
        }

        // The pictures come smaller than the view: no bars, and their room stays where it was, so that the
        // pointer's area keeps its size and place; and larger again, the same.
        shown.picture = QSize(640, 480);
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedScrollX"_s), 0);
        QCOMPARE(seenNamed(window, u"enlargedScrollY"_s), 0);
        QCOMPARE(inScene(pointer), area);
        shown.picture = frame;
        m_control.show(shown);
        QTRY_COMPARE(seenNamed(window, u"enlargedScrollX"_s), 1);
        QCOMPARE(inScene(pointer), area);
        // Fitted, the picture has the whole room again.
        shown.on = false;
        m_control.show(shown);
        QVERIFY(chooseIn(window, u"enlargedScale"_s, u"Fit"_s));
        QTRY_VERIFY(inScene(view).width() > area.width());
        QVERIFY2(m_complaints.isEmpty(), qPrintable(m_complaints.join(u'\n')));
    }
};

int main(int argc, char* argv[])
{
    return runScreensTests<TestScaledScreens>(argc, argv);
}

#include "test_scaled_screens.moc"
