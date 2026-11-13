#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QTimer>

#include <qkvm/input/keys.h>

namespace qkvm::ui {

// Windows mouse capture with an independent escape watchdog. Tests replace every
// operating-system operation; they never register a mouse or confine the real cursor.
class PointerCapture : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    struct System {
        std::function<bool(quintptr)> take;
        std::function<void()> release;
        std::function<bool(const QRect&)> confine;
        std::function<void()> free;
        std::function<bool(quintptr)> foreground;
        std::function<bool(std::uint16_t)> leavePressed;
        std::function<std::optional<QPoint>(quintptr)> movement;
    };

    explicit PointerCapture(System system, QObject* parent = nullptr);
    ~PointerCapture() override;
    bool begin(quintptr window, const QRect& physicalPicture, std::uint16_t leaveKey = input::usageRightCtrl);
    void end();
    bool active() const { return m_held.load(); }
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

    // Absolute tablet/remote-desktop packets must not be interpreted as relative jumps.
    static std::optional<QPoint> relativeDelta(unsigned flags, long x, long y);

signals:
    void moved(int dx, int dy);
    void lost();

private:
    void releasePointer();
    System m_system;
    std::atomic<bool> m_held{false};
    std::atomic<qint64> m_heartbeat{0};
    std::jthread m_watch;
    QTimer m_pulse;
    quintptr m_window = 0;
    quint64 m_turn = 0;
    bool m_registered = false;
    std::uint16_t m_leaveKey = input::usageRightCtrl;
};

PointerCapture::System windowsPointerSystem();

} // namespace qkvm::ui
