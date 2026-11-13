#include "video_tile.h"

#include <QQuickWindow>

#include "video_host.h"

namespace qkvm::ui {

VideoTile::VideoTile(QQuickItem* parent) : QQuickItem(parent) {}

VideoTile::~VideoTile()
{
    if (m_host != nullptr) {
        m_host->forget(this);
    }
}

quint32 VideoTile::stream() const
{
    return m_stream;
}

void VideoTile::setStream(quint32 stream)
{
    if (stream == m_stream) {
        return;
    }
    m_stream = stream;
    emit streamChanged();
    askForAFrame();
}

void VideoTile::setClipTo(QQuickItem* clipTo)
{
    if (clipTo == m_clipTo) {
        return;
    }
    m_clipTo = clipTo;
    emit clipToChanged();
    askForAFrame();
}

void VideoTile::itemChange(ItemChange change, const ItemChangeData& data)
{
    if (change == ItemSceneChange) {
        moveTo(data.window);
    } else if (change == ItemVisibleHasChanged) {
        askForAFrame();
    }
    QQuickItem::itemChange(change, data);
}

void VideoTile::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    askForAFrame();
}

void VideoTile::moveTo(QQuickWindow* window)
{
    VideoHost* host = window != nullptr ? VideoHost::of(window) : nullptr;
    if (host == m_host) {
        return;
    }
    if (m_host != nullptr) {
        m_host->forget(this);
    }
    m_host = host;
    if (m_host != nullptr) {
        m_host->know(this);
    }
}

void VideoTile::hostCame(VideoHost* host)
{
    m_host = host;
    m_host->know(this);
}

void VideoTile::askForAFrame()
{
    // Where the pictures are is read when the window next draws; a tile that moved while nothing
    // else changed has to make it draw.
    if (window() != nullptr) {
        window()->update();
    }
}

} // namespace qkvm::ui
