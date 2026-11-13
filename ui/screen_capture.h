#pragma once

#include <functional>

#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "actions_view.h"
#include "control_view.h"
#include "file_door.h"
#include "wall_view.h"

namespace qkvm::ui {

// Explicit save/copy of this window's displayed remote frame. The image is never
// persisted or put on the clipboard until that request is still permitted.
class ScreenCapture : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qkvm::ui::WallView* view READ view WRITE setView NOTIFY changed)
    Q_PROPERTY(qkvm::ui::ActionsView* actions READ actions WRITE setActions NOTIFY changed)
    Q_PROPERTY(qkvm::ui::ControlView* control READ control WRITE setControl NOTIFY changed)
    Q_PROPERTY(bool possible READ possible NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool holdsWindow READ holdsWindow NOTIFY changed)

public:
    using Done = std::function<void(QImage)>;
    explicit ScreenCapture(QQuickItem* parent = nullptr);
    WallView* view() const { return m_view; }
    ActionsView* actions() const { return m_actions; }
    ControlView* control() const { return m_control; }
    void setView(WallView* view);
    void setActions(ActionsView* actions);
    void setControl(ControlView* control);
    bool possible() const;
    bool busy() const { return m_busy; }
    bool holdsWindow() const { return m_busy || m_choosing; }
    Q_INVOKABLE void save();
    Q_INVOKABLE void copy();

    // Tests supply synthetic pixels, a temporary destination and a private clipboard.
    void captureWith(std::function<void(quint32, Done)> capture) { m_capture = std::move(capture); }
    void copyWith(std::function<bool(const QImage&)> copy) { m_copy = std::move(copy); }
    void setDoor(FileDoor door) { m_door = std::move(door); }

signals:
    void changed();

private:
    bool permitted() const;
    quint32 stream() const;
    void check();
    void cancel();
    void request(bool clipboard);
    void received(quint64 turn, bool clipboard, const QImage& image);
    bool current(quint64 turn) const;
    void finish(const QString& message = {}, bool good = false);
    QPointer<WallView> m_view;
    QPointer<ActionsView> m_actions;
    QPointer<ControlView> m_control;
    std::function<void(quint32, Done)> m_capture;
    std::function<bool(const QImage&)> m_copy;
    FileDoor m_door;
    QTimer m_timeout;
    QString m_machine;
    quint32 m_stream = 0;
    quint64 m_turn = 0;
    bool m_busy = false;
    bool m_choosing = false;
};

} // namespace qkvm::ui
