#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

#include "relative_pointer.h"
#include "screen_capture.h"
#include "win/pointer_capture.h"

using namespace Qt::StringLiterals;
using namespace qkvm::ui;

namespace {

QImage picture()
{
    QImage made(12, 8, QImage::Format_ARGB32);
    made.fill(Qt::red);
    made.setPixelColor(2, 3, Qt::blue);
    return made;
}

struct Screen {
    Screen()
    {
        WallTile tile;
        tile.key = u"a"_s;
        tile.name = u"Test machine"_s;
        tile.state = u"online"_s;
        tile.stream = 7;
        tile.actable = true;
        WallTile second = tile;
        second.key = u"b"_s;
        second.stream = 8;
        wall.machines()->setTiles({tile, second});
        wall.enlarge(u"a"_s);
        actions.setAllowed(true);
        capture.setView(&wall);
        capture.setActions(&actions);
        capture.setControl(&control);
        capture.captureWith([this](quint32 stream, ScreenCapture::Done done) {
            requested = stream;
            pending = std::move(done);
            ++requests;
        });
        capture.copyWith([this](const QImage& image) {
            clipboard = image;
            ++copies;
            return true;
        });
    }
    WallView wall;
    ActionsView actions;
    ControlView control;
    ScreenCapture capture;
    ScreenCapture::Done pending;
    QImage clipboard;
    quint32 requested = 0;
    int requests = 0;
    int copies = 0;
};

struct MouseSystem {
    PointerCapture::System calls()
    {
        return {[this](quintptr) {
                    ++takes;
                    return canTake;
                },
                [this] { ++releases; },
                [this](const QRect&) {
                    ++clips;
                    return canClip;
                },
                [this] { ++frees; },
                [this](quintptr) { return foreground.load(); },
                [this](std::uint16_t key) {
                    watchedKey.store(key);
                    return leave.load();
                },
                [this](quintptr) { return delta; }};
    }
    std::atomic<int> takes{0};
    std::atomic<int> releases{0};
    std::atomic<int> clips{0};
    std::atomic<int> frees{0};
    std::atomic<bool> foreground{true};
    std::atomic<bool> leave{false};
    std::atomic<std::uint16_t> watchedKey{0};
    bool canTake = true;
    bool canClip = true;
    std::optional<QPoint> delta = QPoint(4, -5);
};

} // namespace

class TestConsoleTools : public QObject {
    Q_OBJECT

private slots:
    void screenshotCopiesOnlyAfterAnExplicitRequest()
    {
        Screen screen;
        QVERIFY(screen.capture.possible());
        QCOMPARE(screen.requests, 0);
        screen.wall.setLiveStream(42);
        screen.capture.copy();
        QCOMPARE(screen.requested, quint32{42});
        QVERIFY(screen.capture.busy());
        screen.capture.copy();
        QCOMPARE(screen.requests, 1);
        QCOMPARE(screen.copies, 0);
        screen.pending(picture());
        QCOMPARE(screen.copies, 1);
        QCOMPARE(screen.clipboard, picture());
        QVERIFY(!screen.capture.busy());
        screen.pending(picture());
        QCOMPARE(screen.copies, 1);
    }

    void screenshotSavesARealPngAtTheChosenPath()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"picture.png"_s);
        Screen screen;
        int asked = 0;
        screen.capture.setDoor({[&](const QString& suggested) -> qkvm::base::Result<QString> {
                                    ++asked;
                                    if (!suggested.startsWith(u"qkvm-screenshot-"_s) ||
                                        !suggested.endsWith(u".png"_s)) {
                                        return qkvm::base::Error{"name", "Unexpected filename."};
                                    }
                                    return path;
                                },
                                {}});
        screen.capture.save();
        QCOMPARE(asked, 0);
        screen.pending(picture());
        QCOMPARE(asked, 1);
        QCOMPARE(QImage(path), picture());
        QCOMPARE(screen.copies, 0);
        QVERIFY(!screen.capture.holdsWindow());
    }

    void aPendingScreenshotIsDiscarded_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"machine", "permission", "hidden", "covered", "control", "stream", "display", "card",
                                   "item", "disabled"}) {
            QTest::newRow(reason) << QString::fromLatin1(reason);
        }
    }

    void aPendingScreenshotIsDiscarded()
    {
        QFETCH(QString, reason);
        Screen screen;
        screen.capture.copy();
        QVERIFY(screen.capture.busy());
        if (reason == u"machine")
            screen.wall.enlarge(u"b"_s);
        else if (reason == u"permission")
            screen.actions.setAllowed(false);
        else if (reason == u"hidden")
            screen.wall.setUnseen(true);
        else if (reason == u"covered")
            screen.wall.setCovered(true);
        else if (reason == u"control") {
            ControlView::Shown held;
            held.on = true;
            screen.control.show(held);
        } else if (reason == u"stream")
            screen.wall.setLiveStream(42);
        else if (reason == u"display")
            screen.wall.setDisplaying(true);
        else if (reason == u"card")
            screen.wall.machines()->setTiles({});
        else if (reason == u"item")
            screen.capture.setVisible(false);
        else
            screen.capture.setEnabled(false);
        screen.pending(picture());
        QCOMPARE(screen.copies, 0);
        QVERIFY(!screen.capture.busy());
    }

    void lockingInsideTheSaveDialogDoesNotWriteThePicture()
    {
        Screen screen;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"private.png"_s);
        bool heldWhileLocked = false;
        screen.capture.setDoor({[&](const QString&) -> qkvm::base::Result<QString> {
                                    screen.actions.setAllowed(false);
                                    heldWhileLocked = screen.capture.holdsWindow();
                                    return path;
                                },
                                {}});
        screen.capture.save();
        screen.pending(picture());
        QVERIFY(heldWhileLocked);
        QVERIFY(!QFile::exists(path));
        QVERIFY(!screen.capture.holdsWindow());
    }

    void anEmptyPictureOrCancelledDialogDoesNothing()
    {
        Screen screen;
        int asked = 0;
        screen.capture.setDoor({[&](const QString&) -> qkvm::base::Result<QString> {
                                    ++asked;
                                    return QString();
                                },
                                {}});
        screen.capture.save();
        screen.pending({});
        QCOMPARE(asked, 0);
        screen.capture.save();
        screen.pending(picture());
        QCOMPARE(asked, 1);
        QVERIFY(!screen.capture.busy());
        QCOMPARE(screen.copies, 0);
    }

    void anotherExtensionIsNotSilentlyRenamedOrOverwritten()
    {
        Screen screen;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"picture.jpg"_s);
        screen.capture.setDoor({[&](const QString&) -> qkvm::base::Result<QString> { return path; }, {}});
        screen.capture.save();
        screen.pending(picture());
        QVERIFY(!QFile::exists(path));
        QVERIFY(!QFile::exists(path + u".png"));
        QVERIFY(!screen.capture.busy());
    }

    void aDestroyedScreenshotOwnerDiscardsItsCallback()
    {
        auto screen = std::make_unique<Screen>();
        screen->capture.copy();
        const auto done = screen->pending;
        screen.reset();
        done(picture());
    }

    void aScreenshotThatNeverArrivesTimesOutAndCannotOverwriteTheNextOne()
    {
        Screen screen;
        screen.capture.copy();
        const auto old = screen.pending;
        QTRY_VERIFY_WITH_TIMEOUT(!screen.capture.busy(), 6000);
        screen.capture.copy();
        old(picture());
        QCOMPARE(screen.copies, 0);
        QVERIFY(screen.capture.busy());
        screen.pending(picture());
        QCOMPARE(screen.copies, 1);
    }

    void destroyingTheWindowInsideItsSaveDialogWritesNothing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"cancelled.png"_s);
        auto screen = std::make_unique<Screen>();
        screen->capture.setDoor({[&](const QString&) -> qkvm::base::Result<QString> {
                                     screen.reset();
                                     return path;
                                 },
                                 {}});
        screen->capture.save();
        const auto done = screen->pending;
        done(picture());
        QVERIFY(!QFile::exists(path));
    }

    void rawMouseStartsAndStopsWithoutKeepingThePointer()
    {
        MouseSystem system;
        PointerCapture mouse(system.calls());
        QSignalSpy moves(&mouse, &PointerCapture::moved);
        QVERIFY(mouse.begin(1, QRect(10, 20, 400, 300)));
        MSG event{};
        // NOLINTNEXTLINE(performance-no-int-to-ptr): a synthetic message; never sent to Windows.
        event.hwnd = reinterpret_cast<HWND>(quintptr{1});
        event.message = WM_INPUT;
        event.wParam = RIM_INPUT;
        qintptr result = 0;
        QVERIFY(!mouse.nativeEventFilter({}, &event, &result));
        QCOMPARE(moves.count(), 1);
        QCOMPARE(moves.at(0), (QList<QVariant>{4, -5}));
        event.wParam = RIM_INPUTSINK;
        mouse.nativeEventFilter({}, &event, &result);
        QCOMPARE(moves.count(), 1);
        mouse.end();
        mouse.end();
        event.wParam = RIM_INPUT;
        mouse.nativeEventFilter({}, &event, &result);
        QCOMPARE(moves.count(), 1);
        QCOMPARE(system.frees.load(), 1);
        QCOMPARE(system.releases.load(), 1);
        QVERIFY(!mouse.active());
    }

    void failedCaptureRollsBackTheMouseRegistration()
    {
        MouseSystem system;
        PointerCapture mouse(system.calls());
        system.canClip = false;
        QVERIFY(!mouse.begin(1, QRect(0, 0, 20, 20)));
        QCOMPARE(system.releases.load(), 1);
        QCOMPARE(system.frees.load(), 0);
        system.canClip = true;
        system.foreground.store(false);
        QVERIFY(!mouse.begin(1, QRect(0, 0, 20, 20)));
        QCOMPARE(system.takes.load(), 1);
        system.foreground.store(true);
        system.leave.store(true);
        QVERIFY(!mouse.begin(1, QRect(0, 0, 20, 20)));
        QCOMPARE(system.takes.load(), 1);
    }

    void focusLossAndRightCtrlReleaseThePointer_data()
    {
        QTest::addColumn<bool>("leave");
        QTest::newRow("focus") << false;
        QTest::newRow("Right Ctrl") << true;
    }

    void focusLossAndRightCtrlReleaseThePointer()
    {
        QFETCH(bool, leave);
        MouseSystem system;
        PointerCapture mouse(system.calls());
        QSignalSpy lost(&mouse, &PointerCapture::lost);
        QVERIFY(mouse.begin(1, QRect(0, 0, 20, 20)));
        if (leave)
            system.leave.store(true);
        else
            system.foreground.store(false);
        QTRY_COMPARE_WITH_TIMEOUT(system.frees.load(), 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 1000);
        QCOMPARE(system.releases.load(), 1);
        QVERIFY(!mouse.active());
    }

    void aStalledInterfaceCannotHoldThePointer()
    {
        MouseSystem system;
        PointerCapture mouse(system.calls());
        QVERIFY(mouse.begin(1, QRect(0, 0, 20, 20)));
        // Deliberately do not process Qt events: its heartbeat stops, while the
        // independent watchdog must free the pointer on its own thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(1800));
        QCOMPARE(system.frees.load(), 1);
        QVERIFY(!mouse.active());
        QTRY_COMPARE(system.releases.load(), 1);
    }

    void pointerWatchUsesTheSelectedReleaseKey_data()
    {
        QTest::addColumn<int>("key");
        QTest::newRow("F12") << 0x45;
        QTest::newRow("Scroll Lock") << 0x47;
        QTest::newRow("Right Ctrl") << 0xE4;
    }

    void pointerWatchUsesTheSelectedReleaseKey()
    {
        QFETCH(int, key);
        MouseSystem system;
        PointerCapture mouse(system.calls());
        QVERIFY(mouse.begin(1, QRect(0, 0, 20, 20), static_cast<std::uint16_t>(key)));
        QCOMPARE(system.watchedKey.load(), key);
        system.leave.store(true);
        // Do not process the GUI queue: release must happen on the watchdog thread.
        QElapsedTimer waited;
        waited.start();
        while (system.frees.load() == 0 && waited.elapsed() < 1000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        QCOMPARE(system.frees.load(), 1);
        QVERIFY(!mouse.active());
        QTRY_COMPARE(system.releases.load(), 1);
        QVERIFY(!mouse.begin(1, QRect(0, 0, 20, 20), static_cast<std::uint16_t>(key)));
    }

    void absoluteMousePacketsAndLetterboxBarsAreNotRelativeMotion()
    {
        QVERIFY(!PointerCapture::relativeDelta(MOUSE_MOVE_ABSOLUTE, 65535, 65535));
        const auto relative = PointerCapture::relativeDelta(MOUSE_MOVE_RELATIVE, -12, 35);
        QVERIFY(relative.has_value());
        QCOMPARE(relative.value_or(QPoint()), QPoint(-12, 35));
        QCOMPARE(RelativePointer::pictureArea({800, 600}, {1920, 1080}), QRectF(0, 75, 800, 450));
        QCOMPARE(RelativePointer::pictureArea({800, 600}, {800, 1200}), QRectF(200, 0, 400, 600));
        QVERIFY(RelativePointer::pictureArea({800, 600}, {}).isEmpty());
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    TestConsoleTools tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_console_tools.moc"
