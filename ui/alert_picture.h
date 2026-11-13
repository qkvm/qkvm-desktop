#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// A picture that is no machine's live screen, drawn by QML itself: the one that set off an alert of a
// machine's screen watch (DetailsView::pictureImage). It keeps its shape, as large as fits and in the
// middle; nothing where it has none. (A machine's own screen is the engine's to draw: VideoTile.)
//
// Scaled once to the size it is shown at, in the monitor's own pixels, on a thread beside the interface
// (a picture may have 40 million pixels); and again once for each new size, no more often than that can be
// done (while a window is resized: the scaling under way, and then the last size). Its painting, which
// the interface waits for, only puts what was scaled on the screen; until the new size is there, the
// last one is put in its place as it is.
class AlertPicture : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged)
    // The size it was scaled to last, in the monitor's own pixels; nothing until it was.
    Q_PROPERTY(QSize shown READ shown NOTIFY shownChanged)
    // How often it was scaled: once for each size it came to be shown at.
    Q_PROPERTY(int scalings READ scalings NOTIFY shownChanged)

public:
    explicit AlertPicture(QQuickItem* parent = nullptr);

    QImage image() const { return m_image; }
    void setImage(const QImage& image);
    void paint(QPainter* painter) override;
    QSize shown() const { return m_scaled.size(); }
    int scalings() const { return m_scalings; }

    // Where a picture of this size lies in a place of that size: as large as fits, in the middle.
    static QRectF placeOf(QSizeF place, QSize picture);

signals:
    void imageChanged();
    void shownChanged();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    // Asks for the picture at the size it is shown at now, where it is not of that size yet and no
    // scaling is under way (that one asks again when it is done); and what came of one.
    void scaleAgain();
    void scaledCame(const QImage& scaled, quint64 turn);

    QImage m_image;
    // Raised with each picture: what was scaled of an earlier one is nobody's.
    quint64 m_turn = 0;
    QImage m_scaled;
    bool m_scaling = false;
    int m_scalings = 0;
};

} // namespace qkvm::ui
