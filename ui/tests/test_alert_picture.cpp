#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTest>

#include "alert_picture.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// A picture as large as the server keeps (6000 by 4000): red on its left half, blue on its right.
QImage halves(QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, size.width() / 2, size.height()), Qt::red);
    painter.fillRect(QRect(size.width() / 2, 0, size.width() - size.width() / 2, size.height()), Qt::blue);
    return image;
}

// What the item paints, into a place of its own size.
QImage painted(ui::AlertPicture& picture)
{
    QImage into(picture.size().toSize(), QImage::Format_RGB32);
    into.fill(Qt::black);
    QPainter painter(&into);
    picture.paint(&painter);
    return into;
}

} // namespace

// The picture that set off a screen alert, drawn by QML (ui::AlertPicture): scaled once to the size it is shown at, on
// a thread beside the interface, and again once for each new size; its painting, which the interface waits for, only
// puts that on the screen.
class TestAlertPicture : public QObject {
    Q_OBJECT

private slots:
    void itIsScaledOnceBesideTheInterfaceAndPaintedAsItIs()
    {
        ui::AlertPicture picture;
        picture.setSize(QSizeF(300, 200));
        picture.setImage(halves(QSize(6000, 4000)));
        // Not here: beside the interface, which goes on meanwhile.
        QVERIFY(picture.shown().isEmpty());
        QCOMPARE(picture.scalings(), 0);
        QTRY_COMPARE(picture.shown(), QSize(300, 200));
        QCOMPARE(picture.scalings(), 1);
        // Painted as often as the screen asks, and never scaled for that.
        QElapsedTimer clock;
        clock.start();
        QImage last;
        for (int time = 0; time < 20; ++time) {
            last = painted(picture);
        }
        QVERIFY2(clock.elapsed() < 400, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(picture.scalings(), 1);
        QCOMPARE(last.pixelColor(75, 100), QColor(Qt::red));
        QCOMPARE(last.pixelColor(225, 100), QColor(Qt::blue));
    }

    // Scaled as a whole: each pixel of it on the screen is the mean of all that falls in it, not one taken here and
    // there (fine lines, a third of them white, are a grey a third of the way to white throughout).
    void itIsScaledAsAWholeEachPixelTheMeanOfWhatFallsInIt()
    {
        QImage lines(6000, 4000, QImage::Format_RGB32);
        lines.fill(Qt::black);
        for (int x = 0; x < lines.width(); x += 3) {
            for (int y = 0; y < lines.height(); ++y) {
                lines.setPixel(x, y, qRgb(255, 255, 255));
            }
        }
        ui::AlertPicture picture;
        picture.setSize(QSizeF(300, 200));
        picture.setImage(lines);
        QTRY_COMPARE(picture.shown(), QSize(300, 200));
        const QImage seen = painted(picture);
        for (int x = 0; x < 300; ++x) {
            const int grey = qGray(seen.pixel(x, 100));
            QVERIFY2(grey >= 70 && grey <= 100, qPrintable(u"%1 at %2"_s.arg(grey).arg(x)));
        }
    }

    void aNewSizeIsScaledAgainOnceAndAWindowBeingResizedNoMoreOftenThanItCanBe()
    {
        ui::AlertPicture picture;
        picture.setSize(QSizeF(300, 200));
        picture.setImage(halves(QSize(6000, 4000)));
        QTRY_COMPARE(picture.shown(), QSize(300, 200));
        picture.setSize(QSizeF(600, 400));
        QTRY_COMPARE(picture.shown(), QSize(600, 400));
        QCOMPARE(picture.scalings(), 2);
        QTest::qWait(200);
        QCOMPARE(picture.scalings(), 2);
        // Ten sizes in a row, as a window being resized gives them: the one under way, and the last.
        for (int step = 1; step <= 10; ++step) {
            picture.setSize(QSizeF(600 + 30 * step, 400 + 20 * step));
        }
        QTRY_COMPARE(picture.shown(), QSize(900, 600));
        QVERIFY2(picture.scalings() <= 4, qPrintable(QString::number(picture.scalings())));
        // (In the middle of its place, as large as fits, where that has another shape.)
        picture.setSize(QSizeF(900, 300));
        QTRY_COMPARE(picture.shown(), QSize(450, 300));
        const QImage seen = painted(picture);
        QCOMPARE(seen.pixelColor(100, 150), QColor(Qt::black));
        QCOMPARE(seen.pixelColor(300, 150), QColor(Qt::red));
        QCOMPARE(seen.pixelColor(600, 150), QColor(Qt::blue));
        // Another picture: of that one only.
        picture.setImage(halves(QSize(400, 300)));
        QTRY_COMPARE(picture.shown(), QSize(400, 300));
        picture.setImage({});
        QTRY_VERIFY(picture.shown().isEmpty());
        QCOMPARE(painted(picture).pixelColor(450, 150), QColor(Qt::black));
        // One handed while the one before is being scaled to the same size: that one is nobody's.
        picture.setImage(halves(QSize(6000, 4000)));
        QImage green(600, 400, QImage::Format_RGB32);
        green.fill(Qt::green);
        picture.setImage(green);
        QTRY_COMPARE(picture.shown(), QSize(450, 300));
        QTest::qWait(300);
        QCOMPARE(painted(picture).pixelColor(450, 150), QColor(Qt::green));
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestAlertPicture test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_alert_picture.moc"
