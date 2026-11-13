#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>

#include <qkvm/engine/gpu.h>
#include <qkvm/engine/scene.h>

class QQuickWindow;

namespace qkvm::ui {

class VideoTile;

// The engine in one Qt Quick window (DESIGN.md section 3.6). Qt Quick is given the engine's device
// before the window is first shown; from then on, each time Qt draws the window, the engine draws
// the pictures of the window's VideoTiles first and Qt's own scene goes on top.
//
// Made on the interface thread, before the window is shown, and ended there by whoever made it,
// before the engine's Gpu and before or after the window. The three hooks below run on Qt's
// drawing thread.
class VideoHost : public QObject {
    Q_OBJECT

public:
    VideoHost(engine::Gpu& gpu, QQuickWindow* window);
    ~VideoHost() override;

    // The host of a window, if it has one. A window without (a test's, drawn in software) simply
    // shows no pictures.
    static VideoHost* of(QQuickWindow* window);

    // The window's streams: whoever feeds them adds them here, under the numbers the tiles name.
    engine::Scene& scene() { return *m_scene; }

    // For VideoTile. On the interface thread.
    void know(VideoTile* tile);
    void forget(VideoTile* tile);
    // GUI thread request, fulfilled from the displayed stream on the rendering thread.
    // The callback runs on the GUI thread. Empty means that no picture could be copied.
    void screenshot(engine::StreamId stream, std::function<void(QImage)> done);
    // Where a tile's picture is drawn in its window, in the monitor's own pixels (`scale` of them to
    // each of the window's), and of it only what lies in the item it is clipped to (VideoTile::clipTo,
    // b10); nothing for a tile that names no stream or is not seen.
    static std::optional<engine::Placed> placedOf(const VideoTile& tile, qreal scale);

private:
    void place();
    void draw();
    void frameEnded();
    // A frame is asked for: at once, by the window's drawing thread itself (urgent), or at Qt's own
    // pace through the interface thread.
    void askForAFrame(bool urgent);

    QPointer<QQuickWindow> m_window;
    std::unique_ptr<engine::Scene> m_scene;
    QList<VideoTile*> m_tiles;
    std::atomic<bool> m_asked{false};
    std::atomic<bool> m_urgent{false};
    // What the window's hooks on its drawing thread, and the jobs this host gives that thread, hold
    // of it: a lock that a hook holds while it runs, and under which it looks whether this host is
    // gone. Its own, and held by the hooks and jobs themselves: Qt may be calling one as this host
    // is ended (a frame the drawing thread began by itself, while the window is still shown, as a
    // workspace is put in place), and a job may run after it, as Qt runs what is left when it ends
    // a window's drawing. The end takes the lock once, after a hook under way, to say so.
    struct Hooks {
        std::mutex drawing;
        bool gone = false;
    };
    std::shared_ptr<Hooks> m_hooks = std::make_shared<Hooks>();
    QList<QMetaObject::Connection> m_connections;
    // The window's size in its own pixels at the last sync: a frame the drawing thread begins by
    // itself reads nothing of the window, which the interface thread may be changing. After a
    // resize such a frame may draw into Qt's new size while this is still the old one: the pictures
    // land on the same pixels (the tiles' places are of the same sync), cut to the old size, until
    // the next sync.
    QSize m_size;
    struct Capture {
        engine::StreamId stream = 0;
        std::function<void(QImage)> done;
    };
    std::mutex m_captureLock;
    std::optional<Capture> m_capture;
};

} // namespace qkvm::ui
