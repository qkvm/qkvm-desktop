#include "relative_pointer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QQuickWindow>
#include <QScopedValueRollback>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qkvm::ui {

RelativePointer::RelativePointer(QQuickItem* parent) : QQuickItem(parent)
{
    makeCapture(windowsPointerSystem());
    m_physical = [](QQuickWindow* shown, QRectF local) {
        const qreal scale = shown->effectiveDevicePixelRatio();
        POINT origin{};
        // NOLINTNEXTLINE(performance-no-int-to-ptr): Windows handle from QWindow::winId.
        if (!ClientToScreen(reinterpret_cast<HWND>(shown->winId()), &origin)) {
            return QRect();
        }
        const int left = static_cast<int>(std::ceil(local.left() * scale));
        const int top = static_cast<int>(std::ceil(local.top() * scale));
        const int right = static_cast<int>(std::floor(local.right() * scale));
        const int bottom = static_cast<int>(std::floor(local.bottom() * scale));
        return QRect(origin.x + left, origin.y + top, right - left, bottom - top);
    };
    m_watch.setInterval(100);
    // The toolbar changes the picture's bounds when control begins. Let QML finish
    // that layout before confining the pointer, and never change control from a binding.
    m_start.setSingleShot(true);
    m_start.setInterval(50);
    connect(&m_start, &QTimer::timeout, this, [this] { synchronizeNow(true); });
    connect(&m_watch, &QTimer::timeout, this, &RelativePointer::synchronize);
    connect(this, &QQuickItem::visibleChanged, this, &RelativePointer::synchronize);
    connect(this, &QQuickItem::enabledChanged, this, &RelativePointer::synchronize);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* shown) {
        release(true);
        if (shown != nullptr) {
            connect(shown, &QWindow::activeChanged, this, &RelativePointer::synchronize);
            connect(shown, &QWindow::visibilityChanged, this, &RelativePointer::synchronize);
            connect(shown, &QWindow::xChanged, this, &RelativePointer::synchronize);
            connect(shown, &QWindow::yChanged, this, &RelativePointer::synchronize);
            connect(shown, &QWindow::widthChanged, this, &RelativePointer::synchronize);
            connect(shown, &QWindow::heightChanged, this, &RelativePointer::synchronize);
            connect(shown, &QWindow::screenChanged, this, &RelativePointer::synchronize);
        }
        synchronize();
    });
}

RelativePointer::~RelativePointer()
{
    if (m_control != nullptr) {
        disconnect(m_control, nullptr, this, nullptr);
    }
    release(true);
}

void RelativePointer::makeCapture(PointerCapture::System system)
{
    m_capture = std::make_unique<PointerCapture>(std::move(system));
    connect(m_capture.get(), &PointerCapture::moved, this, [this](int dx, int dy) {
        if (m_started && m_control != nullptr && m_control->isOn() && m_control->isRelative()) {
            m_control->pointerBy(dx, dy);
        }
    });
    connect(m_capture.get(), &PointerCapture::lost, this, [this] { release(true); });
}

void RelativePointer::useSystem(PointerCapture::System system, std::function<QRect(QQuickWindow*, QRectF)> physical)
{
    release(true);
    makeCapture(std::move(system));
    m_physical = std::move(physical);
}

void RelativePointer::setControl(ControlView* control)
{
    if (m_control == control) {
        return;
    }
    release(true);
    if (m_control != nullptr) {
        disconnect(m_control, nullptr, this, nullptr);
    }
    m_control = control;
    if (control != nullptr) {
        connect(control, &ControlView::changed, this, &RelativePointer::synchronize);
        connect(control, &QObject::destroyed, this, [this] { release(false); });
    }
    synchronize();
    emit controlChanged();
}

QRectF RelativePointer::pictureArea(QSizeF tile, QSize picture)
{
    if (tile.width() <= 0 || tile.height() <= 0 || picture.width() <= 0 || picture.height() <= 0) {
        return {};
    }
    const qreal scale = std::min(tile.width() / picture.width(), tile.height() / picture.height());
    const QSizeF size(picture.width() * scale, picture.height() * scale);
    return {(tile.width() - size.width()) / 2, (tile.height() - size.height()) / 2, size.width(), size.height()};
}

QRectF RelativePointer::areaOf(QSizeF tile, QSize picture, const QRectF& placed)
{
    if (placed.isEmpty()) {
        return pictureArea(tile, picture);
    }
    return placed.intersected(QRectF(QPointF(0, 0), tile));
}

void RelativePointer::setPlaced(const QRectF& placed)
{
    if (placed == m_placed) {
        return;
    }
    m_placed = placed;
    emit placedChanged();
    synchronize();
}

void RelativePointer::release(bool tell)
{
    const bool was = std::exchange(m_started, false);
    m_watch.stop();
    m_start.stop();
    m_capture->end();
    m_area = {};
    if (tell && was && m_control != nullptr) {
        const QPointer<ControlView> control = m_control;
        QMetaObject::invokeMethod(
            control,
            [control] {
                if (control != nullptr && control->pointerCaptured()) {
                    control->captureLost();
                }
            },
            Qt::QueuedConnection);
    }
}

void RelativePointer::synchronize()
{
    synchronizeNow(false);
}

void RelativePointer::synchronizeNow(bool begin)
{
    if (m_synchronizing) {
        return;
    }
    const QScopedValueRollback<bool> guard(m_synchronizing, true);
    const bool wanted =
        m_control != nullptr && m_control->isOn() && m_control->isRelative() && !m_control->localInput();
    if (!wanted) {
        release(false);
        return;
    }
    if (!m_started && !begin) {
        if (!m_start.isActive()) {
            m_start.start();
        }
        return;
    }
    QQuickWindow* shown = window();
    if (shown == nullptr || !shown->isActive() || !shown->isVisible() || shown->visibility() == QWindow::Minimized ||
        !isVisible() || !isEnabled()) {
        release(false);
        m_control->captureLost();
        return;
    }
    const QRectF picture = areaOf({width(), height()}, m_control->picture(), m_placed);
    const QRect physical = picture.isEmpty() ? QRect() : m_physical(shown, mapRectToScene(picture));
    if (m_started) {
        if (!m_capture->active() || physical != m_area) {
            release(true);
        }
        return;
    }
    if (physical.isEmpty() || !m_capture->begin(shown->winId(), physical, m_control->leaveKey())) {
        m_control->captureLost();
        return;
    }
    m_started = true;
    m_area = physical;
    m_watch.start();
    m_control->captureReady();
}

void RelativePointer::geometryChange(const QRectF& next, const QRectF& before)
{
    QQuickItem::geometryChange(next, before);
    synchronize();
}

} // namespace qkvm::ui
