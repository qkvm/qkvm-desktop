#include "video_host.h"

#include <cmath>
#include <utility>
#include <vector>

#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QRunnable>

#include <qkvm/base/clock.h>

#include "video_tile.h"

namespace qkvm::ui {

namespace {

// Under this name a window carries its host, for the tiles that come into it to find.
constexpr const char* hostProperty = "qkvmVideoHost";

// The tiles that are in a window already: looked for among what is seen, which is how a window
// holds the cards a model made.
void tellTheTilesUnder(QQuickItem* item, VideoHost* host)
{
    if (auto* tile = qobject_cast<VideoTile*>(item)) {
        tile->hostCame(host);
    }
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        tellTheTilesUnder(child, host);
    }
}

} // namespace

VideoHost::VideoHost(engine::Gpu& gpu, QQuickWindow* window) : m_window(window)
{
    // One device for everything: nothing is copied between the engine's pictures and Qt's drawing.
    window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(gpu.nativeDevice(), gpu.nativeContext()));
    window->setProperty(hostProperty, QVariant::fromValue<QObject*>(this));
    tellTheTilesUnder(window->contentItem(), this);

    m_scene = gpu.makeScene({[this] { askForAFrame(false); }, [this](engine::Fault) { askForAFrame(false); },
                             [this] { askForAFrame(true); }});

    // Direct: each runs on the drawing thread, at the moment Qt says, and not later on this one. Each
    // holds the hooks' own lock while it runs, and does nothing where this host is gone (Hooks).
    const auto hook = [this](void (VideoHost::*what)()) {
        return [this, hooks = m_hooks, what] {
            const std::lock_guard drawing(hooks->drawing);
            if (!hooks->gone) {
                (this->*what)();
            }
        };
    };
    m_connections = {
        connect(window, &QQuickWindow::beforeSynchronizing, window, hook(&VideoHost::place), Qt::DirectConnection),
        connect(window, &QQuickWindow::beforeRenderPassRecording, window, hook(&VideoHost::draw), Qt::DirectConnection),
        connect(window, &QQuickWindow::afterFrameEnd, window, hook(&VideoHost::frameEnded), Qt::DirectConnection),
    };
}

VideoHost::~VideoHost()
{
    // A hook under way on the drawing thread ends first; from here on no hook touches anything of
    // this host, and the hooks are let go of. A job given to the drawing thread that looked just
    // before may still ask the window for a frame, which is no harm: the window outlives its host.
    {
        const std::lock_guard drawing(m_hooks->drawing);
        m_hooks->gone = true;
    }
    for (const QMetaObject::Connection& connection : std::as_const(m_connections)) {
        disconnect(connection);
    }
    // The streams end first, each waiting for its decoding: after this nothing asks for a frame.
    m_scene.reset();
    for (VideoTile* tile : std::as_const(m_tiles)) {
        tile->hostGone();
    }
    if (m_window != nullptr) {
        m_window->setProperty(hostProperty, QVariant());
    }
}

VideoHost* VideoHost::of(QQuickWindow* window)
{
    return qobject_cast<VideoHost*>(window->property(hostProperty).value<QObject*>());
}

void VideoHost::know(VideoTile* tile)
{
    if (!m_tiles.contains(tile)) {
        m_tiles.append(tile);
    }
}

void VideoHost::forget(VideoTile* tile)
{
    m_tiles.removeAll(tile);
}

void VideoHost::screenshot(engine::StreamId stream, std::function<void(QImage)> done)
{
    if (m_window == nullptr || !m_window->isVisible() || stream == 0) {
        done({});
        return;
    }
    std::optional<Capture> previous;
    {
        const std::lock_guard guard(m_captureLock);
        previous = std::exchange(m_capture, Capture{stream, std::move(done)});
    }
    if (previous) {
        previous->done({});
    }
    m_window->update();
}

void VideoHost::place()
{
    // Qt holds the interface thread still while this runs, so the window and its tiles can be read
    // from here.
    if (m_window == nullptr) {
        return;
    }
    const qreal scale = m_window->effectiveDevicePixelRatio();
    m_size = m_window->size() * scale;
    std::vector<engine::Placed> placed;
    placed.reserve(static_cast<std::size_t>(m_tiles.size()));
    for (const VideoTile* tile : std::as_const(m_tiles)) {
        if (const std::optional<engine::Placed> one = placedOf(*tile, scale)) {
            placed.push_back(*one);
        }
    }
    m_scene->place(placed);
}

std::optional<engine::Placed> VideoHost::placedOf(const VideoTile& tile, qreal scale)
{
    if (tile.stream() == 0 || !tile.isVisible() || tile.width() <= 0 || tile.height() <= 0) {
        return std::nullopt;
    }
    // In the monitor's own pixels, and on whole ones: a picture between two pixels is blurred.
    const auto onPixels = [scale](const QRectF& where) {
        const int left = static_cast<int>(std::lround(where.left() * scale));
        const int top = static_cast<int>(std::lround(where.top() * scale));
        const int right = static_cast<int>(std::lround(where.right() * scale));
        const int bottom = static_cast<int>(std::lround(where.bottom() * scale));
        return engine::RectPx{left, top, right - left, bottom - top};
    };
    engine::Placed placed{tile.stream(), onPixels(tile.mapRectToScene(QRectF(0, 0, tile.width(), tile.height())))};
    // Of a picture larger than its view, only what lies in the view (b10).
    if (const QQuickItem* view = tile.clipTo(); view != nullptr && view->width() > 0 && view->height() > 0) {
        placed.clip = onPixels(view->mapRectToScene(QRectF(0, 0, view->width(), view->height())));
    }
    return placed;
}

void VideoHost::draw()
{
    if (m_window == nullptr) {
        return;
    }
    // Qt is told that its own state on the device is touched, and sets it again afterwards.
    m_window->beginExternalCommands();
    m_scene->drawUnder({m_size.width(), m_size.height()});
    std::optional<Capture> capture;
    {
        const std::lock_guard asked(m_captureLock);
        capture = std::exchange(m_capture, std::nullopt);
    }
    if (capture) {
        QImage image;
        if (const auto shot = m_scene->screenshot(capture->stream)) {
            const auto& value = shot.value();
            image = QImage(value.pixels.data(), value.size.width, value.size.height, qsizetype{value.size.width} * 4,
                           QImage::Format_ARGB32)
                        .copy();
        }
        QMetaObject::invokeMethod(
            this, [done = std::move(capture->done), image = std::move(image)] { done(image); }, Qt::QueuedConnection);
    }
    m_window->endExternalCommands();
}

void VideoHost::frameEnded()
{
    m_scene->shown(base::steadyNowUs());
}

void VideoHost::askForAFrame(bool urgent)
{
    // From any thread, and often: one request is enough until it has been passed on, and an urgent
    // one makes the request that is passed on urgent.
    if (urgent) {
        m_urgent.store(true);
    }
    if (m_asked.exchange(true)) {
        return;
    }
    // Urgent (a machine shown large or controlled): passed on to the window's drawing thread, as a
    // job of its own there that asks it for a frame. Asked on that thread, Qt draws one without
    // waiting for the interface thread (no polish of the scene, no sync with it, no wait for the
    // platform's update timer), which is all a new picture needs; what else changed in the window
    // asks for a frame of its own. From the picture decoded to the frame that takes it: 5.8 ms
    // through the interface thread, 0.1 ms so, for a 4K60 film on the card (measured).
    // The request itself still goes by the interface thread's event loop, once: the job
    // is handed over from there, because handing it over reads Qt's own list of windows, which only
    // that thread changes. (The job's update() on the drawing thread reads that list as well, as
    // Qt's own animators do from there.) A job that looks after this host is gone does nothing.
    // Where Qt draws on the interface thread itself, the job runs at once and asks there. Otherwise (a wall's tiles):
    // at Qt's own pace, which gathers the pictures of many tiles into one frame (a wall of 64 drew 9.5 frames a second
    // so, and 12.7 with every picture drawn at once; part 9 as well).
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_asked.store(false);
            const bool urgent = m_urgent.exchange(false);
            if (m_window == nullptr) {
                return;
            }
            if (urgent) {
                m_window->scheduleRenderJob(QRunnable::create([window = m_window.data(), hooks = m_hooks] {
                                                bool gone = false;
                                                {
                                                    const std::lock_guard drawing(hooks->drawing);
                                                    gone = hooks->gone;
                                                }
                                                if (!gone) {
                                                    window->update();
                                                }
                                            }),
                                            QQuickWindow::NoStage);
            } else {
                m_window->update();
            }
        },
        Qt::QueuedConnection);
}

} // namespace qkvm::ui
