#include "window_keeper.h"

#include <algorithm>
#include <utility>

#include <QGuiApplication>
#include <QScreen>

namespace qkvm::ui {

namespace {

// As much of the window's top edge has to be on a screen: wide enough to take hold of with the
// pointer, and as high as a title bar.
constexpr int holdWidth = 120;
constexpr int holdHeight = 24;

QList<QRect> screensOfWindows()
{
    QList<QRect> free;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (const QScreen* screen : screens) {
        free.append(screen->availableGeometry());
    }
    return free;
}

} // namespace

bool canBePutBack(const core::WindowPlace& place, const QList<QRect>& screens)
{
    const QRect top(place.x, place.y, place.width, holdHeight);
    return std::ranges::any_of(screens, [&](const QRect& screen) {
        const QRect seen = top.intersected(screen);
        return seen.width() >= holdWidth && seen.height() >= holdHeight;
    });
}

void bringBack(QWindow& window)
{
    window.setVisible(true);
    if ((window.windowStates() & Qt::WindowMinimized) != 0) {
        window.setWindowStates(window.windowStates() & ~Qt::WindowMinimized);
    }
    window.raise();
    window.requestActivate();
}

WindowKeeper::WindowKeeper(QWindow& window, core::WindowSlot& kept, const std::function<QList<QRect>()>& screens,
                           int restMs, QObject* parent)
    : QObject(parent), m_window(window), m_kept(kept)
{
    if (const std::optional<core::WindowPlace> was = m_kept.remembered().place;
        was && canBePutBack(*was, screens ? screens() : screensOfWindows())) {
        m_place = *was;
        m_placeKnown = true;
        m_window.setGeometry(m_place.x, m_place.y, m_place.width, m_place.height);
    }
    m_rest.setSingleShot(true);
    m_rest.setInterval(restMs);
    connect(&m_rest, &QTimer::timeout, this, &WindowKeeper::remember);
    // Whatever moves, the window is looked at once it has come to rest.
    const auto moved = [this] { m_rest.start(); };
    connect(&m_window, &QWindow::xChanged, this, moved);
    connect(&m_window, &QWindow::yChanged, this, moved);
    connect(&m_window, &QWindow::widthChanged, this, moved);
    connect(&m_window, &QWindow::heightChanged, this, moved);
    connect(&m_window, &QWindow::visibilityChanged, this, &WindowKeeper::note);
    // Closed: how it is at this moment is how it was.
    m_window.installEventFilter(this);
}

void WindowKeeper::show()
{
    if (m_placeKnown && m_place.fullScreen) {
        // What it goes back to from there is the window's own to know (Main.qml).
        m_window.setProperty("wasMaximized", m_place.maximized);
        m_window.showFullScreen();
    } else if (m_placeKnown && m_place.maximized) {
        m_window.showMaximized();
    } else {
        m_window.show();
    }
}

void WindowKeeper::restore(const std::optional<core::WindowPlace>& place, const std::function<QList<QRect>()>& screens)
{
    m_rest.stop();
    m_window.showNormal();
    const auto available = screens ? screens() : screensOfWindows();
    if (place && canBePutBack(*place, available)) {
        m_place = *place;
    } else {
        const auto free = available.value(0, QRect(0, 0, 1280, 800));
        m_place = {free.x() + 24, free.y() + 24, std::min(1280, free.width() - 48), std::min(800, free.height() - 48),
                   false,         false};
    }
    m_placeKnown = true;
    m_window.setGeometry(m_place.x, m_place.y, m_place.width, m_place.height);
    show();
    remember();
}

bool WindowKeeper::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == &m_window && event->type() == QEvent::Close) {
        m_rest.stop();
        remember();
    }
    return QObject::eventFilter(watched, event);
}

void WindowKeeper::note(QWindow::Visibility now)
{
    if (now == QWindow::Windowed || now == QWindow::Maximized) {
        m_place.maximized = now == QWindow::Maximized;
        m_place.fullScreen = false;
    } else if (now == QWindow::FullScreen) {
        // Under it the window is what it was before: maximized, or of its own size.
        m_place.fullScreen = true;
    } else {
        // Minimized, or not shown: as it was before that.
        return;
    }
    m_rest.start();
}

void WindowKeeper::remember()
{
    // Its place is the one it has while it is of its own size: maximized, over the whole screen,
    // minimized or not shown, it is where it was before.
    if (m_window.visibility() == QWindow::Windowed) {
        m_place.x = m_window.x();
        m_place.y = m_window.y();
        m_place.width = m_window.width();
        m_place.height = m_window.height();
        m_placeKnown = true;
    }
    // A window that has not been seen at its own size yet has no place to write down.
    if (!m_placeKnown) {
        return;
    }
    core::WallWindow remembered = m_kept.remembered();
    remembered.place = m_place;
    // A PC whose settings cannot be written has its window where Windows puts it, the next time.
    static_cast<void>(m_kept.remember(remembered));
}

} // namespace qkvm::ui
