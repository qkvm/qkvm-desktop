#include "screen_capture.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QQuickWindow>

#include "video_host.h"
#include "win/file_dialogs.h"

namespace qkvm::ui {

ScreenCapture::ScreenCapture(QQuickItem* parent) : QQuickItem(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(5000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(tr("The displayed picture could not be copied.")); });
    connect(this, &QQuickItem::visibleChanged, this, &ScreenCapture::check);
    connect(this, &QQuickItem::enabledChanged, this, &ScreenCapture::check);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* shown) {
        cancel();
        if (shown != nullptr) {
            connect(shown, &QWindow::visibilityChanged, this, &ScreenCapture::check);
        }
    });
}

void ScreenCapture::setView(WallView* view)
{
    if (m_view == view) {
        return;
    }
    cancel();
    if (m_view != nullptr) {
        disconnect(m_view, nullptr, this, nullptr);
    }
    m_view = view;
    if (view != nullptr) {
        connect(view, &WallView::enlargedChanged, this, &ScreenCapture::check);
        connect(view, &WallView::enlargedCardChanged, this, &ScreenCapture::check);
        connect(view, &WallView::liveStreamChanged, this, &ScreenCapture::check);
        connect(view, &WallView::unseenChanged, this, &ScreenCapture::check);
        connect(view, &WallView::displayingChanged, this, &ScreenCapture::check);
        connect(view, &WallView::coveredChanged, this, &ScreenCapture::check);
        connect(view, &QObject::destroyed, this, &ScreenCapture::cancel);
    }
    emit changed();
}

void ScreenCapture::setActions(ActionsView* actions)
{
    if (m_actions == actions) {
        return;
    }
    cancel();
    if (m_actions != nullptr) {
        disconnect(m_actions, nullptr, this, nullptr);
    }
    m_actions = actions;
    if (actions != nullptr) {
        connect(actions, &ActionsView::allowedChanged, this, &ScreenCapture::check);
        connect(actions, &QObject::destroyed, this, &ScreenCapture::cancel);
    }
    emit changed();
}

void ScreenCapture::setControl(ControlView* control)
{
    if (m_control == control) {
        return;
    }
    cancel();
    if (m_control != nullptr) {
        disconnect(m_control, nullptr, this, nullptr);
    }
    m_control = control;
    if (control != nullptr) {
        connect(control, &ControlView::changed, this, &ScreenCapture::check);
        connect(control, &QObject::destroyed, this, &ScreenCapture::cancel);
    }
    emit changed();
}

quint32 ScreenCapture::stream() const
{
    return m_view == nullptr           ? 0
           : m_view->liveStream() != 0 ? m_view->liveStream()
                                       : m_view->enlargedCard().value(QStringLiteral("stream")).toUInt();
}

bool ScreenCapture::permitted() const
{
    return m_view != nullptr && m_actions != nullptr && m_actions->allowed() && !m_view->enlarged().isEmpty() &&
           m_view->enlargedCard().value(QStringLiteral("actable")).toBool() && !m_view->isUnseen() &&
           !m_view->displaying() && !m_view->covered() && isVisible() && isEnabled() &&
           (m_control == nullptr || (!m_control->isOn() && !m_control->isAsking())) &&
           (window() == nullptr || (window()->isVisible() && window()->visibility() != QWindow::Minimized));
}

bool ScreenCapture::possible() const
{
    return !holdsWindow() && permitted() && stream() != 0;
}

void ScreenCapture::check()
{
    if (m_busy && !current(m_turn)) {
        cancel();
        return;
    }
    emit changed();
}

void ScreenCapture::cancel()
{
    ++m_turn;
    m_timeout.stop();
    m_busy = false;
    m_machine.clear();
    m_stream = 0;
    emit changed();
}

bool ScreenCapture::current(quint64 turn) const
{
    return turn == m_turn && m_busy && permitted() && m_machine == m_view->enlarged() && m_stream == stream();
}

void ScreenCapture::save()
{
    request(false);
}

void ScreenCapture::copy()
{
    request(true);
}

void ScreenCapture::request(bool clipboard)
{
    if (!possible()) {
        return;
    }
    m_busy = true;
    m_machine = m_view->enlarged();
    m_stream = stream();
    const quint64 turn = ++m_turn;
    m_timeout.start();
    const QPointer<ScreenCapture> guarded(this);
    emit changed();
    if (guarded == nullptr || !current(turn)) {
        return;
    }
    const auto done = [self = QPointer<ScreenCapture>(this), turn, clipboard](const QImage& image) {
        if (self != nullptr) {
            self->received(turn, clipboard, image);
        }
    };
    if (m_capture) {
        m_capture(m_stream, done);
    } else if (window() != nullptr && VideoHost::of(window()) != nullptr) {
        VideoHost::of(window())->screenshot(m_stream, done);
    } else {
        done({});
    }
}

void ScreenCapture::received(quint64 turn, bool clipboard, const QImage& image)
{
    if (!current(turn)) {
        return;
    }
    m_timeout.stop();
    if (image.isNull()) {
        finish(tr("There is no displayed picture to copy yet."));
        return;
    }
    if (clipboard) {
        const QPointer<ScreenCapture> self(this);
        bool copied = false;
        if (m_copy) {
            copied = m_copy(image);
        } else if (QClipboard* board = QGuiApplication::clipboard(); board != nullptr) {
            board->setImage(image);
            copied = board->ownsClipboard();
        }
        if (self != nullptr && current(turn)) {
            finish(copied ? tr("Screenshot copied to clipboard.") : tr("The screenshot could not be copied."), copied);
        }
        return;
    }
    const QString suggested = QStringLiteral("qkvm-screenshot-%1.png")
                                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const FileDoor door = m_door.whereToSave ? m_door : windowsFileDoor(window());
    const QPointer<ScreenCapture> self(this);
    m_choosing = true;
    emit changed();
    if (self == nullptr) {
        return;
    }
    if (!current(turn)) {
        m_choosing = false;
        emit changed();
        return;
    }
    const auto chosen = door.whereToSave(suggested);
    // The native dialog runs an event loop: locking, changing machine, or destroying
    // this window while it is open must not write a picture after it returns.
    if (self == nullptr) {
        return;
    }
    m_choosing = false;
    emit changed();
    if (self == nullptr || !current(turn)) {
        return;
    }
    if (!chosen) {
        finish(tr("The screenshot file could not be chosen."));
        return;
    }
    if (chosen.value().isEmpty()) {
        finish();
        return;
    }
    const QString path = asWindowsTakesIt(chosen.value());
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        finish(tr("Choose a filename ending in .png."));
        return;
    }
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        finish(tr("The screenshot could not be prepared."));
        return;
    }
    if (!current(turn)) {
        return;
    }
    const auto written = writeWholeFile(path, png);
    finish(written ? tr("Screenshot saved.") : tr("The screenshot could not be saved."), written.ok());
}

void ScreenCapture::finish(const QString& message, bool good)
{
    const QPointer<ScreenCapture> self(this);
    cancel();
    if (self != nullptr && !message.isEmpty() && m_actions != nullptr) {
        m_actions->tell(message, good);
    }
}

} // namespace qkvm::ui
