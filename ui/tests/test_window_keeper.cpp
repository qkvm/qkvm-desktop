#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

#include <qkvm/core/settings.h>

#include "window_keeper.h"

using namespace Qt::StringLiterals;
using namespace qkvm;

namespace {

// Two monitors side by side, each by the part of it that is free of the taskbar.
QList<QRect> twoScreens()
{
    return {QRect(0, 0, 1920, 1040), QRect(1920, 0, 1280, 984)};
}

core::WindowPlace at(int x, int y, int width, int height)
{
    core::WindowPlace place;
    place.x = x;
    place.y = y;
    place.width = width;
    place.height = height;
    return place;
}

// A window as the settings hold one, said in one line: where it was, or nowhere yet, and of its
// wall the group, the order, the columns and whether the groups take turns.
core::WallWindow windowAt(const std::optional<core::WindowPlace>& place, const QString& group = {},
                          const QString& order = {}, int columns = 0, bool rotates = false)
{
    core::WallWindow window;
    window.place = place;
    window.group = group;
    window.order = order;
    window.columns = columns;
    window.rotates = rotates;
    return window;
}

// What a settings file holds of the first window just now; no place at all where it holds none.
core::WindowPlace keptIn(const QString& file)
{
    return core::Settings(file).remembered().windows.value(0).place.value_or(core::WindowPlace{});
}

} // namespace

// The window is where it was, and as it was: against Qt's platform without a screen, where a window
// is moved, maximized and closed as on a desktop.
class TestWindowKeeper : public QObject {
    Q_OBJECT

private slots:
    void aWindowIsBroughtBackAsItWas()
    {
        QWindow window;
        // Not shown yet: brought back, it is there.
        ui::bringBack(window);
        QVERIFY(window.isVisible());
        window.showMaximized();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // Minimized, as its button in the title bar does it: back to what it was, and not to a
        // window of its own size.
        window.setWindowStates(window.windowStates() | Qt::WindowMinimized);
        QTRY_VERIFY((window.windowStates() & Qt::WindowMinimized) != 0);
        ui::bringBack(window);
        QTRY_VERIFY((window.windowStates() & Qt::WindowMinimized) == 0);
        QVERIFY((window.windowStates() & Qt::WindowMaximized) != 0);
        QVERIFY(window.isVisible());

        // Out of sight: the same.
        window.hide();
        QVERIFY(!window.isVisible());
        ui::bringBack(window);
        QVERIFY(window.isVisible());
        QVERIFY((window.windowStates() & Qt::WindowMaximized) != 0);
    }

    void aPlaceIsUsedOnlyWhereItsTopEdgeCanBeTakenHoldOf_data()
    {
        QTest::addColumn<QRect>("where");
        QTest::addColumn<bool>("canBe");
        QTest::newRow("in the middle of the first monitor") << QRect(300, 200, 1280, 800) << true;
        QTest::newRow("on the second monitor") << QRect(2000, 100, 1000, 700) << true;
        QTest::newRow("across the two") << QRect(1500, 50, 1000, 700) << true;
        QTest::newRow("larger than the monitor it is on") << QRect(-200, 0, 2400, 1300) << true;
        QTest::newRow("hanging out at the bottom") << QRect(300, 900, 1280, 800) << true;
        QTest::newRow("on a monitor that is gone") << QRect(4000, 100, 1280, 800) << false;
        QTest::newRow("left of everything") << QRect(-3000, 100, 1280, 800) << false;
        QTest::newRow("its top edge above the monitor") << QRect(300, -200, 1280, 800) << false;
        QTest::newRow("its top edge under the taskbar") << QRect(300, 1039, 1280, 800) << false;
        QTest::newRow("its top edge half under the taskbar") << QRect(300, 1030, 1280, 800) << false;
        QTest::newRow("a corner of it, too narrow to take hold of") << QRect(3081, 100, 1280, 800) << false;
        QTest::newRow("a corner of it, wide enough") << QRect(3080, 100, 1280, 800) << true;
        QTest::newRow("nothing at all") << QRect(300, 200, 0, 0) << false;
    }

    void aPlaceIsUsedOnlyWhereItsTopEdgeCanBeTakenHoldOf()
    {
        QFETCH(QRect, where);
        QFETCH(bool, canBe);
        QCOMPARE(ui::canBePutBack(at(where.x(), where.y(), where.width(), where.height()), twoScreens()), canBe);
        // A PC without a monitor has nowhere to put it.
        QVERIFY(!ui::canBePutBack(at(where.x(), where.y(), where.width(), where.height()), {}));
    }

    void aWindowThatWasNeverClosedIsWhereItIsPutAndIsRememberedThere()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        QWindow window;
        window.setGeometry(40, 60, 640, 400);
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 60);
        // Nothing was remembered: it is left as it was made.
        QCOMPARE(window.geometry(), QRect(40, 60, 640, 400));
        QVERIFY(!settings.remembered().windows.value(0).place.has_value());
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.visibility(), QWindow::Windowed);
        // Once it has come to rest, where it is is written down.
        QTRY_VERIFY(core::Settings(file).remembered().windows.value(0).place.has_value());
        QCOMPARE(keptIn(file), at(40, 60, 640, 400));
    }

    void aSecondWindowHasAPlaceOfItsOwn()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        {
            core::Settings settings(file);
            core::WindowSlot firstKept(settings, 0);
            core::WindowSlot secondKept(settings, 1);
            QWindow first;
            QWindow second;
            first.setGeometry(40, 60, 640, 400);
            second.setGeometry(2000, 100, 900, 600);
            ui::WindowKeeper firstKeeper(first, firstKept, twoScreens, 60);
            ui::WindowKeeper secondKeeper(second, secondKept, twoScreens, 60);
            firstKeeper.show();
            secondKeeper.show();
            QVERIFY(QTest::qWaitForWindowExposed(&second));
            QTRY_COMPARE(core::Settings(file).remembered().windows.size(), 2);
            QTRY_COMPARE(core::Settings(file).remembered().windows.at(0).place.value_or(core::WindowPlace{}),
                         at(40, 60, 640, 400));
            QCOMPARE(core::Settings(file).remembered().windows.at(1).place.value_or(core::WindowPlace{}),
                     at(2000, 100, 900, 600));
        }
        // The next start: each where it was, the second on the second monitor.
        core::Settings settings(file);
        core::WindowSlot secondKept(settings, 1);
        QWindow second;
        const ui::WindowKeeper secondKeeper(second, secondKept, twoScreens, 60);
        QCOMPARE(second.geometry(), QRect(2000, 100, 900, 600));
    }

    void theWindowIsWhereItWas()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        core::Remembered remembered;
        remembered.server = u"http://192.168.1.10:8787/"_s;
        remembered.windows = {windowAt(at(2000, 100, 1000, 700))};
        QVERIFY(settings.remember(remembered).ok());

        QWindow window;
        window.setGeometry(40, 60, 640, 400);
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 60);
        // Before it is shown at all.
        QCOMPARE(window.geometry(), QRect(2000, 100, 1000, 700));
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.visibility(), QWindow::Windowed);
        QCOMPARE(window.geometry(), QRect(2000, 100, 1000, 700));

        // Moved and made another size: written down once it has come to rest, and not while it moves.
        window.setPosition(2100, 150);
        window.resize(900, 650);
        QCOMPARE(keptIn(file), at(2000, 100, 1000, 700));
        QTRY_COMPARE(keptIn(file), at(2100, 150, 900, 650));
        // With it, nothing else that is remembered is lost.
        QCOMPARE(core::Settings(file).remembered().server, u"http://192.168.1.10:8787/"_s);
        // Whichever way it moves or grows, by itself.
        window.setX(2150);
        QTRY_COMPARE(keptIn(file), at(2150, 150, 900, 650));
        window.setY(170);
        QTRY_COMPARE(keptIn(file), at(2150, 170, 900, 650));
        window.setWidth(950);
        QTRY_COMPARE(keptIn(file), at(2150, 170, 950, 650));
        window.setHeight(600);
        QTRY_COMPARE(keptIn(file), at(2150, 170, 950, 600));
    }

    void aPlaceThatNoScreenShowsIsNotUsed()
    {
        const QTemporaryDir folder;
        core::Settings settings(folder.filePath(u"settings.json"_s));
        core::Remembered remembered;
        // (It was on a third monitor, which is not there any more; and it was maximized.)
        core::WindowPlace wasThere = at(4000, 100, 1000, 700);
        wasThere.maximized = true;
        remembered.windows = {windowAt(wasThere)};
        QVERIFY(settings.remember(remembered).ok());
        QWindow window;
        window.setGeometry(40, 60, 640, 400);
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 60);
        QCOMPARE(window.geometry(), QRect(40, 60, 640, 400));
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // Neither where it was nor as it was: nothing of what was remembered fits this desk.
        QCOMPARE(window.visibility(), QWindow::Windowed);
    }

    void aMaximizedWindowIsMaximizedAgainWithItsOwnSizeUnderIt()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        {
            core::Settings settings(file);
            QWindow window;
            window.setGeometry(300, 200, 800, 500);
            core::WindowSlot part(settings, 0);
            ui::WindowKeeper keeper(window, part, twoScreens, 60);
            keeper.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTRY_VERIFY(core::Settings(file).remembered().windows.value(0).place.has_value());
            window.showMaximized();
            QTRY_COMPARE(window.visibility(), QWindow::Maximized);
            QTRY_VERIFY(keptIn(file).maximized);
            // What is written down is the place it has when it is of its own size, not the desktop's.
            core::WindowPlace kept = at(300, 200, 800, 500);
            kept.maximized = true;
            QCOMPARE(keptIn(file), kept);
        }
        // The next start.
        core::Settings settings(file);
        QWindow window;
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 60);
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.visibility(), QWindow::Maximized);
        // Its own size under that is the one it had: it was given it before it was shown.
        QTest::qWait(200);
        core::WindowPlace kept = at(300, 200, 800, 500);
        kept.maximized = true;
        QCOMPARE(keptIn(file), kept);
        // Made its own size again, that is how it is the next time. (Windows then puts it where
        // it was before it was maximized, which is the place it was given: looked at in the real
        // program. Qt's platform without a screen, which this runs on, puts it nowhere in
        // particular, so the test moves it as a desktop would.)
        window.showNormal();
        window.setGeometry(310, 210, 820, 520);
        QTRY_COMPARE(window.visibility(), QWindow::Windowed);
        QTRY_COMPARE(keptIn(file), at(310, 210, 820, 520));
    }

    void aWindowThatFilledAScreenFillsItAgainAndGoesBackToWhatItWasBefore()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        {
            core::Settings settings(file);
            QWindow window;
            window.setGeometry(2000, 100, 800, 500);
            core::WindowSlot part(settings, 0);
            ui::WindowKeeper keeper(window, part, twoScreens, 60);
            keeper.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTRY_VERIFY(core::Settings(file).remembered().windows.value(0).place.has_value());
            // Maximized, and then over the whole screen, one right after the other: both are known.
            window.showMaximized();
            window.showFullScreen();
            QTRY_COMPARE(window.visibility(), QWindow::FullScreen);
            QTRY_VERIFY(core::Settings(file).remembered().windows.value(0).place.has_value() &&
                        keptIn(file).fullScreen);
            core::WindowPlace kept = at(2000, 100, 800, 500);
            kept.maximized = true;
            kept.fullScreen = true;
            QCOMPARE(keptIn(file), kept);
        }
        {
            // The next start: over the whole screen again, and the window is told what it was under that.
            core::Settings settings(file);
            QWindow window;
            core::WindowSlot part(settings, 0);
            ui::WindowKeeper keeper(window, part, twoScreens, 60);
            QCOMPARE(window.geometry(), QRect(2000, 100, 800, 500));
            keeper.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QCOMPARE(window.visibility(), QWindow::FullScreen);
            QCOMPARE(window.property("wasMaximized").toBool(), true);
            // Back to a window of its own size: that is how it is the next time.
            window.showNormal();
            window.setGeometry(2000, 100, 800, 500);
            QTRY_COMPARE(window.visibility(), QWindow::Windowed);
            QTRY_COMPARE(keptIn(file), at(2000, 100, 800, 500));
        }
        // Over the whole screen from its own size: it goes back to that.
        core::Settings settings(file);
        QWindow window;
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 60);
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.visibility(), QWindow::Windowed);
        window.showFullScreen();
        QTRY_VERIFY(keptIn(file).fullScreen);
        QVERIFY(!keptIn(file).maximized);
        core::Settings again(file);
        QWindow next;
        core::WindowSlot nextPart(again, 0);
        ui::WindowKeeper nextKeeper(next, nextPart, twoScreens, 60);
        nextKeeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&next));
        QCOMPARE(next.visibility(), QWindow::FullScreen);
        QCOMPARE(next.property("wasMaximized").toBool(), false);
    }

    void aMinimizedOrHiddenWindowIsRememberedAsItWasBefore()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        core::Settings settings(file);
        QWindow window;
        window.setGeometry(300, 200, 800, 500);
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 60);
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(core::Settings(file).remembered().windows.value(0).place.has_value());
        window.showMinimized();
        QTRY_COMPARE(window.visibility(), QWindow::Minimized);
        // (Windows gives a minimized window a place far off every monitor: that is not its place.)
        window.setPosition(-32000, -32000);
        QTest::qWait(250);
        QCOMPARE(keptIn(file), at(300, 200, 800, 500));
        window.hide();
        QTest::qWait(250);
        QCOMPARE(keptIn(file), at(300, 200, 800, 500));
        // Closed while it is so: still as it was before.
        QVERIFY(window.close());
        QCOMPARE(keptIn(file), at(300, 200, 800, 500));
    }

    void aWindowThatIsClosedIsWrittenDownAtOnce()
    {
        const QTemporaryDir folder;
        const QString file = folder.filePath(u"settings.json"_s);
        // (This keeper waits ten seconds for a window to come to rest.)
        core::Settings settings(file);
        QWindow window;
        window.setGeometry(300, 200, 800, 500);
        core::WindowSlot part(settings, 0);
        ui::WindowKeeper keeper(window, part, twoScreens, 10'000);
        keeper.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.setPosition(350, 250);
        QTest::qWait(100);
        QVERIFY(!core::Settings(file).remembered().windows.value(0).place.has_value());
        QVERIFY(window.close());
        QCOMPARE(keptIn(file), at(350, 250, 800, 500));
    }
};

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestWindowKeeper test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_window_keeper.moc"
