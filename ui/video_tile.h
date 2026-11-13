#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

class VideoHost;

// Where one stream's picture is in a window. It paints nothing itself: the engine draws the picture
// underneath everything QML draws (DESIGN.md section 3.6), exactly where this item is. So whatever
// QML puts over it shows on the picture, and nothing opaque may lie between it and the window's
// own background.
class VideoTile : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(quint32 stream READ stream WRITE setStream NOTIFY streamChanged)
    // The view the picture is shown in where it may be larger than that (the enlarged view at a scale,
    // b10): of the picture only what lies inside that item is drawn. None: all of the tile.
    Q_PROPERTY(QQuickItem* clipTo READ clipTo WRITE setClipTo NOTIFY clipToChanged)

public:
    explicit VideoTile(QQuickItem* parent = nullptr);
    ~VideoTile() override;

    // Which of the engine's streams is drawn here; 0 for none.
    quint32 stream() const;
    void setStream(quint32 stream);
    QQuickItem* clipTo() const { return m_clipTo; }
    void setClipTo(QQuickItem* clipTo);

    // For VideoHost: the window was given its host after this tile was in it; and the window's
    // host is gone, and with it the pictures.
    void hostCame(VideoHost* host);
    void hostGone() { m_host = nullptr; }

signals:
    void streamChanged();
    void clipToChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void moveTo(QQuickWindow* window);
    void askForAFrame();

    quint32 m_stream = 0;
    VideoHost* m_host = nullptr;
    QPointer<QQuickItem> m_clipTo;
};

} // namespace qkvm::ui
