#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <QFile>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QRect>
#include <QSGRendererInterface>
#include <QScreen>
#include <QTest>
#include <QThread>

#include <qkvm/base/clock.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/scene.h>

#include "video_host.h"
#include "video_tile.h"

using namespace qkvm;

// The engine in a Qt Quick window, as the program has it, on Windows' software adapter: what a new
// picture costs the window.
class TestVideoHost : public QObject {
    Q_OBJECT

private:
    static std::unique_ptr<engine::Gpu> softwareGpu()
    {
        engine::Gpu::Options options;
        options.software = true;
        base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create(options);
        return made ? std::move(made).value() : nullptr;
    }

    static std::vector<std::byte> picture(const char* name = "desktop-preview-480/001.jpg")
    {
        QFile file(QString::fromStdString((std::filesystem::path(QKVM_TESTDATA) / name).string()));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QByteArray bytes = file.readAll();
        std::vector<std::byte> copy(static_cast<std::size_t>(bytes.size()));
        std::memcpy(copy.data(), bytes.constData(), copy.size());
        return copy;
    }

private slots:
    void initTestCase() { QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11); }

    // A new picture of a machine shown large is drawn in a frame that the window's drawing thread asks
    // for itself: no sync with the interface thread comes before it (where Qt draws on a thread of
    // its own; where it draws on the interface thread, there is no such thing to leave out). A wall
    // tile's is drawn at Qt's own pace, through the interface thread.
    void aNewPictureIsDrawnWithoutTheInterfaceThreadsSync_data()
    {
        QTest::addColumn<bool>("large");
        QTest::newRow("shown large") << true;
        QTest::newRow("a wall's tile") << false;
    }

    void aNewPictureIsDrawnWithoutTheInterfaceThreadsSync()
    {
        QFETCH(const bool, large);
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::vector<std::byte> jpeg = picture();
        QVERIFY2(!jpeg.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");

        // A small window in a corner of the screen that takes no keyboard, for a moment: Qt draws a
        // window that is shown, and nothing that has no screen.
        if (QGuiApplication::primaryScreen() == nullptr) {
            QSKIP("No screen to show a window on.");
        }
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        window.setGeometry(screen.right() - 320, screen.bottom() - 180, 320, 180);
        ui::VideoHost host(*gpu, &window);
        // (Given its window after it is made, as QML gives an item its parent: made with it, it
        // would never hear of the window's host.)
        const auto tile = std::make_unique<ui::VideoTile>();
        tile->setParentItem(window.contentItem());
        tile->setSize({320, 180});
        tile->setStream(7);
        engine::Stream& stream = host.scene().add(7, engine::Codec::Still);
        stream.shownAt({320, 180}, large ? engine::Importance::ShownLarge : engine::Importance::Preview);

        std::atomic<int> syncs{0};
        std::atomic<int> frames{0};
        std::atomic<bool> ownThread{false};
        const Qt::HANDLE interfaceThread = QThread::currentThreadId();
        QObject::connect(
            &window, &QQuickWindow::beforeSynchronizing, &window,
            [&] {
                ++syncs;
                ownThread = QThread::currentThreadId() != interfaceThread;
            },
            Qt::DirectConnection);
        QObject::connect(&window, &QQuickWindow::afterFrameEnd, &window, [&] { ++frames; }, Qt::DirectConnection);
        window.show();
        if (!QTest::qWaitForWindowExposed(&window)) {
            QSKIP("No window can be shown here (no desktop to show it on).");
        }
        QTRY_VERIFY(frames.load() > 0);
        // The window comes to rest: no frame for a while.
        int seen = -1;
        while (seen != frames.load()) {
            seen = frames.load();
            QTest::qWait(300);
        }

        const int syncsBefore = syncs.load();
        stream.push({jpeg, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().shown, std::uint64_t{1});
        if (!ownThread.load()) {
            QSKIP("Qt draws this window on the interface thread here.");
        }
        if (!large) {
            QVERIFY(syncs.load() > syncsBefore);
            window.close();
            return;
        }
        QCOMPARE(syncs.load(), syncsBefore);
        // And the picture is in the frame: from its decoding to the frame that took it, no wait for
        // the interface thread's pace.
        QVERIFY2(stream.stats().readyToDrawnMs < 50, qPrintable(QString::number(stream.stats().readyToDrawnMs)));
        window.close();
    }

    // The window's host is ended while the window is shown and its drawing thread draws new pictures
    // of a machine shown large, frame after frame, as fast as they come: a frame under way ends
    // first, and none after it touches the engine that is gone. (What this can catch it catches by
    // chance: a frame that is under way at that moment; under the address sanitizer, whatever it
    // touches that is gone.)
    void theHostEndsWhileItsWindowDrawsNewPictures()
    {
        if (QGuiApplication::primaryScreen() == nullptr) {
            QSKIP("No screen to show a window on.");
        }
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        // Two by turns: the same still again is let go by the engine since M6 2c, and would draw nothing.
        const std::vector<std::byte> jpeg = picture();
        const std::vector<std::byte> next = picture("desktop-preview-480/002.jpg");
        QVERIFY2(!jpeg.empty() && !next.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        for (int round = 0; round < 5; ++round) {
            QQuickWindow window;
            window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
            const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
            window.setGeometry(screen.right() - 160, screen.bottom() - 90, 160, 90);
            auto host = std::make_unique<ui::VideoHost>(*gpu, &window);
            const auto tile = std::make_unique<ui::VideoTile>();
            tile->setParentItem(window.contentItem());
            tile->setSize({160, 90});
            tile->setStream(9);
            engine::Stream& stream = host->scene().add(9, engine::Codec::Still);
            stream.shownAt({160, 90}, engine::Importance::ShownLarge);
            window.show();
            if (!QTest::qWaitForWindowExposed(&window)) {
                QSKIP("No window can be shown here (no desktop to show it on).");
            }
            std::atomic<bool> stop{false};
            std::thread pusher([&] {
                for (bool other = false; !stop.load(); other = !other) {
                    stream.push({other ? next : jpeg, base::steadyNowUs(), true});
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
            QTRY_VERIFY(stream.stats().shown > 3);
            stop.store(true);
            pusher.join();
            // Frames for the last pictures are still asked for and under way.
            host.reset();
            QTest::qWait(100);
            window.close();
        }
    }
};

// On Windows' own screen, as the program is: the tests' platform without a screen draws nothing of a
// Qt Quick window's own (it never gets to a render pass, which is where the engine draws). And Qt
// draws the window on a thread of its own, as it does in the program's windows.
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "windows");
    qputenv("QSG_RENDER_LOOP", "threaded");
    QGuiApplication app(argc, argv);
    TestVideoHost test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_video_host.moc"
