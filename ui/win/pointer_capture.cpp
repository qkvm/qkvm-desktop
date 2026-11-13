#include "win/pointer_capture.h"
#include "win/leave_key.h"

#include <chrono>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>

#include <qkvm/base/clock.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace qkvm::ui {

PointerCapture::PointerCapture(System system, QObject* parent) : QObject(parent), m_system(std::move(system))
{
    m_pulse.setInterval(100);
    connect(&m_pulse, &QTimer::timeout, this, [this] { m_heartbeat.store(base::steadyNowUs()); });
}

PointerCapture::~PointerCapture()
{
    end();
}

bool PointerCapture::begin(quintptr window, const QRect& physicalPicture, std::uint16_t leaveKey)
{
    end();
    m_leaveKey = supportedLeaveKey(leaveKey);
    if (window == 0 || physicalPicture.isEmpty() || !m_system.foreground(window) || m_system.leavePressed(m_leaveKey) ||
        !m_system.take(window)) {
        return false;
    }
    m_registered = true;
    if (!m_system.confine(physicalPicture)) {
        end();
        return false;
    }
    m_window = window;
    m_held.store(true);
    m_heartbeat.store(base::steadyNowUs());
    m_pulse.start();
    QCoreApplication::instance()->installNativeEventFilter(this);
    const quint64 turn = ++m_turn;
    m_watch = std::jthread([this, window, turn](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            // This runs independently of Qt: the release key, another foreground window or
            // an interface that stopped responding frees the cursor even if Qt is stuck.
            if (!m_system.foreground(window) || m_system.leavePressed(m_leaveKey) ||
                base::steadyNowUs() - m_heartbeat.load() > 1'500'000) {
                releasePointer();
                QMetaObject::invokeMethod(
                    this,
                    [this, turn] {
                        if (turn == m_turn) {
                            end();
                            emit lost();
                        }
                    },
                    Qt::QueuedConnection);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    return true;
}

void PointerCapture::releasePointer()
{
    if (m_held.exchange(false)) {
        m_system.free();
    }
}

void PointerCapture::end()
{
    ++m_turn;
    m_pulse.stop();
    if (m_watch.joinable()) {
        m_watch.request_stop();
        m_watch.join();
    }
    releasePointer();
    if (m_registered) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
        m_system.release();
        m_registered = false;
    }
    m_window = 0;
}

std::optional<QPoint> PointerCapture::relativeDelta(unsigned flags, long x, long y)
{
    if ((flags & MOUSE_MOVE_ABSOLUTE) != 0) {
        return std::nullopt;
    }
    return QPoint(static_cast<int>(x), static_cast<int>(y));
}

bool PointerCapture::nativeEventFilter(const QByteArray& /*type*/, void* message, qintptr* /*result*/)
{
    const auto* event = static_cast<const MSG*>(message);
    if (!active() || event == nullptr || reinterpret_cast<quintptr>(event->hwnd) != m_window ||
        event->message != WM_INPUT || GET_RAWINPUT_CODE_WPARAM(event->wParam) != RIM_INPUT) {
        return false;
    }
    if (!m_system.foreground(m_window) || m_system.leavePressed(m_leaveKey)) {
        end();
        emit lost();
        return false;
    }
    const auto delta = m_system.movement(static_cast<quintptr>(event->lParam));
    if (delta && !delta->isNull()) {
        emit moved(delta->x(), delta->y());
    }
    // Windows/Qt still perform WM_INPUT cleanup and deliver ordinary buttons/wheel.
    return false;
}

PointerCapture::System windowsPointerSystem()
{
    struct Owned {
        HWND window = nullptr;
        RECT clip{};
    };
    const auto owned = std::make_shared<Owned>();
    const auto registered = []() -> std::optional<std::vector<RAWINPUTDEVICE>> {
        UINT count = 0;
        const UINT asked = GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
        if ((asked == std::numeric_limits<UINT>::max() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) || count > 256) {
            return std::nullopt;
        }
        std::vector<RAWINPUTDEVICE> devices(count);
        if (count != 0) {
            const UINT got = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
            if (got == std::numeric_limits<UINT>::max() || got > devices.size()) {
                return std::nullopt;
            }
            devices.resize(got);
        }
        return devices;
    };
    return {[owned, registered](quintptr window) {
                const auto devices = registered();
                if (!devices) {
                    return false;
                }
                for (const auto& device : devices.value()) {
                    if (device.usUsagePage == 1 && device.usUsage == 2) {
                        return false; // Do not replace another component's mouse registration.
                    }
                }
                // NOLINTNEXTLINE(performance-no-int-to-ptr): Windows window handle from QWindow::winId.
                const RAWINPUTDEVICE mouse{1, 2, 0, reinterpret_cast<HWND>(window)};
                if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) == 0) {
                    return false;
                }
                owned->window = mouse.hwndTarget;
                return true;
            },
            [owned, registered] {
                if (const auto devices = registered()) {
                    for (const auto& device : devices.value()) {
                        if (device.usUsagePage == 1 && device.usUsage == 2 && device.hwndTarget == owned->window &&
                            device.dwFlags == 0) {
                            const RAWINPUTDEVICE mouse{1, 2, RIDEV_REMOVE, nullptr};
                            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
                            break;
                        }
                    }
                }
                owned->window = nullptr;
            },
            [owned](const QRect& rectangle) {
                const RECT clip{rectangle.x(), rectangle.y(), rectangle.x() + rectangle.width(),
                                rectangle.y() + rectangle.height()};
                if (ClipCursor(&clip) == 0) {
                    return false;
                }
                // Windows may intersect a partly off-screen rectangle with the
                // desktop. Remember the rectangle it actually applied for release.
                if (GetClipCursor(&owned->clip) == 0) {
                    ClipCursor(nullptr);
                    return false;
                }
                return true;
            },
            [owned] {
                RECT now{};
                // A different application's newer confinement is not ours to release.
                if (GetClipCursor(&now) != 0 && EqualRect(&now, &owned->clip) != 0) {
                    ClipCursor(nullptr);
                }
            },
            [](quintptr window) { return reinterpret_cast<quintptr>(GetForegroundWindow()) == window; },
            [](std::uint16_t key) { return (GetAsyncKeyState(leaveKeyOfWindows(key)) & 0x8000) != 0; },
            [](quintptr handle) -> std::optional<QPoint> {
                RAWINPUT input{};
                UINT size = sizeof(input);
                // NOLINTNEXTLINE(performance-no-int-to-ptr): WM_INPUT's raw input handle.
                const UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &input, &size,
                                                 sizeof(RAWINPUTHEADER));
                if (got == std::numeric_limits<UINT>::max() || got < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) ||
                    input.header.dwType != RIM_TYPEMOUSE) {
                    return std::nullopt;
                }
                return PointerCapture::relativeDelta(input.data.mouse.usFlags, input.data.mouse.lLastX,
                                                     input.data.mouse.lLastY);
            }};
}

} // namespace qkvm::ui
