#include "alert_picture.h"

#include <algorithm>

#include <QCoreApplication>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QThreadPool>

namespace qkvm::ui {

AlertPicture::AlertPicture(QQuickItem* parent) : QQuickPaintedItem(parent) {}

void AlertPicture::setImage(const QImage& image)
{
    if (image == m_image) {
        return;
    }
    m_image = image;
    ++m_turn;
    // (Another picture's is no longer shown.)
    if (!m_scaled.isNull()) {
        m_scaled = {};
        emit shownChanged();
    }
    emit imageChanged();
    update();
    scaleAgain();
}

QRectF AlertPicture::placeOf(QSizeF place, QSize picture)
{
    if (place.width() <= 0 || place.height() <= 0 || picture.width() <= 0 || picture.height() <= 0) {
        return {};
    }
    const qreal scale = std::min(place.width() / picture.width(), place.height() / picture.height());
    const QSizeF size(picture.width() * scale, picture.height() * scale);
    return {(place.width() - size.width()) / 2, (place.height() - size.height()) / 2, size.width(), size.height()};
}

void AlertPicture::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        scaleAgain();
    }
}

void AlertPicture::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickPaintedItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged || change == ItemSceneChange) {
        scaleAgain();
    }
}

void AlertPicture::scaleAgain()
{
    if (m_scaling) {
        return;
    }
    const qreal ratio = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize wanted = (placeOf(size(), m_image.size()).size() * ratio).toSize();
    if (m_image.isNull() || wanted.isEmpty() || wanted == m_scaled.size()) {
        return;
    }
    m_scaling = true;
    // Beside the interface, which goes on meanwhile; told back on it.
    QThreadPool::globalInstance()->start([self = QPointer<AlertPicture>(this), image = m_image, wanted, turn = m_turn] {
        QImage scaled = image.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, scaled = std::move(scaled), turn] {
                if (self != nullptr) {
                    self->scaledCame(scaled, turn);
                }
            },
            Qt::QueuedConnection);
    });
}

void AlertPicture::scaledCame(const QImage& scaled, quint64 turn)
{
    m_scaling = false;
    if (turn == m_turn && !scaled.isNull()) {
        m_scaled = scaled;
        ++m_scalings;
        emit shownChanged();
        update();
    }
    // (Once more where its size, or the picture, is another by now.)
    scaleAgain();
}

void AlertPicture::paint(QPainter* painter)
{
    const QRectF where = placeOf(size(), m_image.size());
    if (where.isEmpty() || m_scaled.isNull()) {
        return;
    }
    // One of its pixels to each of the monitor's, where it is of the size it is shown at.
    painter->drawImage(where, m_scaled);
}

} // namespace qkvm::ui
