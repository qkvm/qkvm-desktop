#include <array>
#include <utility>

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QTest>

#include "mark.h"

using namespace qkvm;

// The program's mark, drawn: for the window's icon and for the one in the notification area.
class TestMark : public QObject {
    Q_OBJECT

private:
    // The colour at a place of the mark's own grid of 16.
    static QColor at(const QImage& image, double x, double y)
    {
        return image.pixelColor(static_cast<int>(x * image.width() / 16), static_cast<int>(y * image.height() / 16));
    }

    // For whoever wants to look at the marks: with QKVM_SCREENSHOTS set to a folder, they are kept
    // there as pictures, small as the taskbar has them and large.
    static void keepForLookingAt()
    {
        const QString folder = qEnvironmentVariable("QKVM_SCREENSHOTS");
        if (folder.isEmpty()) {
            return;
        }
        QVERIFY(QDir().mkpath(folder));
        const std::array<std::pair<const char*, ui::MarkDot>, 3> marks{
            {{"mark", ui::MarkDot::None}, {"mark-warn", ui::MarkDot::Warn}, {"mark-bad", ui::MarkDot::Bad}}};
        for (const auto& [name, dot] : marks) {
            for (const int size : {24, 256}) {
                const QString file = QStringLiteral("%1-%2.png").arg(QLatin1StringView(name)).arg(size);
                QVERIFY(ui::markImage(size, dot).save(QDir(folder).filePath(file)));
            }
        }
    }

private slots:
    void theMarkIsTheOfficialQOnABlueSquare_data()
    {
        QTest::addColumn<int>("size");
        QTest::newRow("as the taskbar has it") << 16;
        QTest::newRow("on a monitor at one and a half") << 24;
        QTest::newRow("large") << 256;
    }

    void theMarkIsTheOfficialQOnABlueSquare()
    {
        QFETCH(int, size);
        const QImage mark = ui::markImage(size);
        QCOMPARE(mark.size(), QSize(size, size));
        // The middle of each screen, and the ground between them.
        for (const QPointF& middle : {QPointF(5.5, 4.3), QPointF(10.5, 4.3), QPointF(5.5, 8.2), QPointF(10.5, 9.5)}) {
            QCOMPARE(at(mark, middle.x(), middle.y()), QColor(Qt::white));
        }
        QVERIFY(at(mark, 8, 8).blue() > at(mark, 8, 8).red());
        QVERIFY(at(mark, 8, 1.5).blue() > at(mark, 8, 1.5).red());
        QVERIFY(at(mark, 8, 1.5).red() > at(mark, 8, 14.5).red());
        // Its corners are round: at the very corner there is next to nothing, and a little
        // further in the ground.
        QVERIFY(mark.pixelColor(0, 0).alpha() < 40);
        QVERIFY(mark.pixelColor(size - 1, size - 1).alpha() < 40);
        QCOMPARE(mark.pixelColor(size / 4, size / 4).alpha(), 255);
    }

    void aDotInItsCornerSaysThatSomethingIsUp()
    {
        const QImage well = ui::markImage(32);
        const QImage look = ui::markImage(32, ui::MarkDot::Warn);
        const QImage wrong = ui::markImage(32, ui::MarkDot::Bad);
        // Where the dot is, the lower right screen shows through where there is none.
        QCOMPARE(at(well, 11.5, 11.5), QColor(Qt::white));
        QCOMPARE(at(look, 11.5, 11.5), QColor(0xf5, 0x9e, 0x0b));
        QCOMPARE(at(wrong, 11.5, 11.5), QColor(0xef, 0x44, 0x44));
        // The rest of the mark is as it is without one.
        for (const QImage& dotted : {look, wrong}) {
            QCOMPARE(at(dotted, 5, 5), QColor(Qt::white));
            QCOMPARE(at(dotted, 11, 5), QColor(Qt::white));
            QCOMPARE(at(dotted, 5, 8), QColor(Qt::white));
            QCOMPARE(at(dotted, 8, 1.5), at(well, 8, 1.5));
        }
        keepForLookingAt();
    }

    void aSizeThatIsNoneStillGivesAPicture()
    {
        QCOMPARE(ui::markImage(0).size(), QSize(1, 1));
        QCOMPARE(ui::markImage(-5).size(), QSize(1, 1));
    }
};

QTEST_MAIN(TestMark)
#include "test_mark.moc"
