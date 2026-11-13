#pragma once

#include <functional>
#include <memory>

#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "control_view.h"
#include "win/pointer_capture.h"

namespace qkvm::ui {

// The picture's mouse area. Capture exists only while this visible, active window
// owns remote control in relative mode. Changing its physical bounds releases it.
class RelativePointer : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qkvm::ui::ControlView* control READ control WRITE setControl NOTIFY controlChanged)
    // Where the picture lies in this item, where it is shown at a scale of its own (b10) and may be
    // larger than the item; nothing: fitted into it, as the engine draws it.
    Q_PROPERTY(QRectF placed READ placed WRITE setPlaced NOTIFY placedChanged)

public:
    explicit RelativePointer(QQuickItem* parent = nullptr);
    ~RelativePointer() override;
    ControlView* control() const { return m_control; }
    void setControl(ControlView* control);
    QRectF placed() const { return m_placed; }
    void setPlaced(const QRectF& placed);
    static QRectF pictureArea(QSizeF tile, QSize picture);
    // What of the picture is seen in an item of this size, which the pointer is held to: fitted
    // (`placed` empty), all of it; at a scale, the part of it that lies in the item.
    static QRectF areaOf(QSizeF tile, QSize picture, const QRectF& placed);
    void useSystem(PointerCapture::System system, std::function<QRect(QQuickWindow*, QRectF)> physical);

signals:
    void controlChanged();
    void placedChanged();

protected:
    void geometryChange(const QRectF& next, const QRectF& before) override;

private:
    void synchronize();
    void synchronizeNow(bool begin);
    void release(bool tell);
    void makeCapture(PointerCapture::System system);
    QPointer<ControlView> m_control;
    std::unique_ptr<PointerCapture> m_capture;
    std::function<QRect(QQuickWindow*, QRectF)> m_physical;
    QTimer m_watch;
    QTimer m_start;
    QRect m_area;
    QRectF m_placed;
    bool m_started = false;
    bool m_synchronizing = false;
};

} // namespace qkvm::ui
