#include <cmath>

#include <QSignalSpy>
#include <QTest>

#include <QQuickItem>

#include "control_view.h"
#include "picture_scale.h"
#include "relative_pointer.h"
#include "video_host.h"
#include "video_tile.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

// b10: how large a machine's picture is in its enlarged view, where it lies there at every scale, and
// where the pointer is in it: in numbers, as the screen and what is sent to the machine have them.
class TestPictureScale : public QObject {
    Q_OBJECT

private slots:
    void theScalesAreThePages()
    {
        QCOMPARE(ui::pictureScales(),
                 (QStringList{u"fit"_s, u"50"_s, u"75"_s, u"100"_s, u"125"_s, u"140"_s, u"200"_s}));
        QCOMPARE(ui::scaleOf(u"fit"_s), 0.0);
        QCOMPARE(ui::scaleOf(u"50"_s), 0.5);
        QCOMPARE(ui::scaleOf(u"75"_s), 0.75);
        QCOMPARE(ui::scaleOf(u"100"_s), 1.0);
        QCOMPARE(ui::scaleOf(u"125"_s), 1.25);
        QCOMPARE(ui::scaleOf(u"140"_s), 1.4);
        QCOMPARE(ui::scaleOf(u"200"_s), 2.0);
        for (const QString& none : {u"150"_s, u""_s, u"100 "_s, u"1e2"_s, u"-50"_s}) {
            QCOMPARE(ui::scaleOf(none), 0.0);
        }
    }

    // 100 % is one of the monitor's pixels to each of the machine's, whatever Windows' scaling: at
    // 150 % a picture of 1920 pixels is 1280 of the window's own.
    void atOneHundredPercentEachOfTheMachinesPixelsIsOneOfTheMonitors()
    {
        QCOMPARE(ui::drawnAt({1920, 1080}, 1, 1.5), QSizeF(1280, 720));
        QCOMPARE(ui::drawnAt({1920, 1080}, 1, 1), QSizeF(1920, 1080));
        QCOMPARE(ui::drawnAt({1920, 1080}, 1.4, 1), QSizeF(2688, 1512));
        QCOMPARE(ui::drawnAt({1920, 1080}, 2, 1.25), QSizeF(3072, 1728));
        // (On whole pixels of the monitor: 1366 by 0.75 is 1024.5, drawn on 1025 of them.)
        QCOMPARE(ui::drawnAt({1366, 768}, 0.75, 1.25), QSizeF(1025 / 1.25, 576 / 1.25));
        QVERIFY(ui::drawnAt({0, 0}, 1, 1).isEmpty());
        QVERIFY(ui::drawnAt({1920, 1080}, 0, 1).isEmpty());
        QVERIFY(ui::drawnAt({1920, 1080}, 1, 0).isEmpty());
    }

    void wherePictureLiesAtEveryScale_data()
    {
        QTest::addColumn<QSizeF>("view");
        QTest::addColumn<QSize>("picture");
        QTest::addColumn<double>("scale");
        QTest::addColumn<double>("ratio");
        QTest::addColumn<QPointF>("scrolled");
        QTest::addColumn<QRectF>("lies");
        QTest::newRow("fit, bars above and below")
            << QSizeF(1000, 600) << QSize(1920, 1080) << 0.0 << 1.0 << QPointF() << QRectF(0, 18.75, 1000, 562.5);
        QTest::newRow("fit, bars beside")
            << QSizeF(1000, 600) << QSize(1440, 1080) << 0.0 << 1.5 << QPointF(50, 50) << QRectF(100, 0, 800, 600);
        QTest::newRow("50 %, in the middle")
            << QSizeF(1000, 600) << QSize(1920, 1080) << 0.5 << 1.0 << QPointF(40, 40) << QRectF(20, 30, 960, 540);
        QTest::newRow("100 % at 150 %, scrolled")
            << QSizeF(1000, 600) << QSize(1920, 1080) << 1.0 << 1.5 << QPointF(100, 50) << QRectF(-100, -50, 1280, 720);
        QTest::newRow("200 %, scrolled") << QSizeF(1000, 600) << QSize(1920, 1080) << 2.0 << 1.0 << QPointF(300, 200)
                                         << QRectF(-300, -200, 3840, 2160);
        QTest::newRow("200 %, scrolled beyond its ends") << QSizeF(1000, 600) << QSize(1920, 1080) << 2.0 << 1.0
                                                         << QPointF(5000, -10) << QRectF(-2840, 0, 3840, 2160);
        QTest::newRow("125 %, larger each way")
            << QSizeF(1200, 900) << QSize(1024, 768) << 1.25 << 1.0 << QPointF(40, 30) << QRectF(-40, -30, 1280, 960);
        QTest::newRow("50 %, wider and lower")
            << QSizeF(1600, 900) << QSize(3840, 1080) << 0.5 << 1.0 << QPointF(100, 70) << QRectF(-100, 180, 1920, 540);
        QTest::newRow("140 % at 125 %") << QSizeF(1000, 600) << QSize(1280, 1024) << 1.4 << 1.25 << QPointF(10, 20)
                                        << QRectF(-10, -20, 1792 / 1.25, 1433.6 / 1.25);
    }

    void wherePictureLiesAtEveryScale()
    {
        QFETCH(QSizeF, view);
        QFETCH(QSize, picture);
        QFETCH(double, scale);
        QFETCH(double, ratio);
        QFETCH(QPointF, scrolled);
        QFETCH(QRectF, lies);
        const QRectF found = ui::pictureIn(view, picture, scale, ratio, scrolled);
        const auto near = [](double one, double other) { return std::abs(one - other) < 1.0; };
        QVERIFY2(near(found.left(), lies.left()) && near(found.top(), lies.top()) &&
                     near(found.width(), lies.width()) && near(found.height(), lies.height()),
                 qPrintable(
                     u"%1, %2, %3 by %4"_s.arg(found.left()).arg(found.top()).arg(found.width()).arg(found.height())));
    }

    // Every pixel of the machine's screen is where the pointer over it says, at every scale and
    // every place the view is scrolled to: what is sent is the machine's pixel, to a hundredth of one.
    void thePointerIsWhereItIsInThePictureAtEveryScale()
    {
        const QSize picture(1920, 1080);
        const QSizeF view(1000, 600);
        for (const QString& choice : ui::pictureScales()) {
            for (const double ratio : {1.0, 1.25, 1.5, 2.0}) {
                for (const QPointF scrolled : {QPointF(0, 0), QPointF(333, 77), QPointF(100000, 100000)}) {
                    const QRectF lies = ui::pictureIn(view, picture, ui::scaleOf(choice), ratio, scrolled);
                    QVERIFY(!lies.isEmpty());
                    for (const QPoint pixel : {QPoint(0, 0), QPoint(959, 539), QPoint(1919, 1079), QPoint(17, 1000)}) {
                        const QPointF centre((pixel.x() + 0.5) / picture.width(), (pixel.y() + 0.5) / picture.height());
                        const QPointF at(lies.left() + centre.x() * lies.width(),
                                         lies.top() + centre.y() * lies.height());
                        const QPointF said = ui::placeIn(at, lies);
                        QVERIFY2(std::abs(said.x() * picture.width() - (pixel.x() + 0.5)) < 0.01 &&
                                     std::abs(said.y() * picture.height() - (pixel.y() + 0.5)) < 0.01,
                                 qPrintable(u"%1 at %2: %3, %4"_s.arg(choice).arg(ratio).arg(said.x()).arg(said.y())));
                    }
                }
            }
        }
        // Beside the picture it is outside it, on the right side; with no picture, nowhere.
        const QRectF fitted = ui::pictureIn(view, picture, 0, 1, {});
        QVERIFY(ui::placeIn({500, 5}, fitted).y() < 0);
        QVERIFY(ui::placeIn({500, 595}, fitted).y() > 1);
        QCOMPARE(ui::placeIn({500, 300}, QRectF()), QPointF(-1, -1));
    }

    // What the screen hands over for the pointer at a scale is where the picture lies: said as the place
    // in it, each way 0 to 1 over it.
    void thePointerOverAPictureAtAScaleIsSaidByWhereItLies()
    {
        ui::ControlView view;
        const QSignalSpy moved(&view, &ui::ControlView::pointerMoved);
        const QRectF lies = ui::pictureIn({1000, 600}, {1920, 1080}, 2, 1, {300, 200});
        view.pointerOver(500, 300, lies.left(), lies.top(), lies.width(), lies.height());
        QCOMPARE(moved.count(), 1);
        QCOMPARE(moved.last().at(0).toDouble(), 800.0 / 3840);
        QCOMPARE(moved.last().at(1).toDouble(), 500.0 / 2160);
        view.pointerOver(500, 300, 0, 0, 0, 0);
        QCOMPARE(moved.last().at(0).toDouble(), -1.0);
    }

    // In the relative mode the pointer is held to what is seen of the picture: at a scale, the part of
    // it in the view, and never the part that is scrolled out of it (the bars over it are this PC's).
    void theRelativePointerIsHeldToWhatIsSeenOfThePicture()
    {
        // Fitted: as before.
        QCOMPARE(ui::RelativePointer::areaOf({1000, 600}, {1920, 1080}, {}), QRectF(0, 18.75, 1000, 562.5));
        // At a scale, larger than the view: the view.
        QCOMPARE(ui::RelativePointer::areaOf({1000, 600}, {1920, 1080}, QRectF(-300, -200, 3840, 2160)),
                 QRectF(0, 0, 1000, 600));
        // Smaller, in the middle: the picture.
        QCOMPARE(ui::RelativePointer::areaOf({1000, 600}, {1920, 1080}, QRectF(20, 30, 960, 540)),
                 QRectF(20, 30, 960, 540));
        // Larger one way: the part in sight.
        QCOMPARE(ui::RelativePointer::areaOf({1600, 900}, {3840, 1080}, QRectF(-100, 180, 1920, 540)),
                 QRectF(0, 180, 1600, 540));
    }

    // The engine is told to draw a picture larger than its view only inside the view, in the monitor's
    // own pixels: over the bars around the view nothing of it is drawn. Without a view, the whole tile.
    void aPictureLargerThanItsViewIsDrawnOnlyInsideIt()
    {
        QQuickItem window;
        QQuickItem view(&window);
        view.setPosition({0, 100});
        view.setSize({1000, 600});
        ui::VideoTile tile(&view);
        tile.setPosition({-300, -200});
        tile.setSize({3840, 2160});
        tile.setStream(7);
        std::optional<engine::Placed> placed = ui::VideoHost::placedOf(tile, 1.5);
        QVERIFY(placed.has_value());
        engine::Placed one = placed.value_or(engine::Placed{});
        QCOMPARE(one.stream, engine::StreamId{7});
        QCOMPARE(one.where, (engine::RectPx{-450, -150, 5760, 3240}));
        QCOMPARE(one.clip, (engine::RectPx{}));
        tile.setClipTo(&view);
        placed = ui::VideoHost::placedOf(tile, 1.5);
        QVERIFY(placed.has_value());
        one = placed.value_or(engine::Placed{});
        QCOMPARE(one.where, (engine::RectPx{-450, -150, 5760, 3240}));
        QCOMPARE(one.clip, (engine::RectPx{0, 150, 1500, 900}));
        // (A view that is no longer there clips nothing; nor is a tile without a stream placed.)
        tile.setStream(0);
        QVERIFY(!ui::VideoHost::placedOf(tile, 1.5).has_value());
    }
};

QTEST_MAIN(TestPictureScale)
#include "test_picture_scale.moc"
