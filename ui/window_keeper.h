#pragma once

#include <functional>

#include <QEvent>
#include <QList>
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QWindow>

#include <qkvm/core/settings.h>

namespace qkvm::ui {

// Whether a window that was at `place` can be put there again, on these screens (of each, the part
// that is free of the taskbar): enough of its top edge is on one of them to take hold of it by. A
// monitor that was unplugged since, or set to another resolution, would otherwise leave the window
// where nobody can reach it.
bool canBePutBack(const core::WindowPlace& place, const QList<QRect>& screens);

// Brings a window back into sight and to the front, as it was before it was minimized or put out
// of sight: filling the desktop, or a screen, or neither. For whoever clicked the program's icon,
// or started the program a second time.
void bringBack(QWindow& window);

// A window of the program's is where it was when the program was last closed, and as it was: of
// its own size, filling the desktop, or filling a screen. A wall on a monitor nobody sits at comes
// back as it was left after the PC was switched off and on.
//
// What is remembered is the window's place while it is of its own size, and which of the other
// two it is, if either: so a window that was closed maximized is maximized again, and has its old
// size under that. It is written down a moment after the window has come to rest, and when the
// window is closed. A window that is minimized is remembered as it was before.
//
// On the interface thread. The window, and its part of what is remembered, outlive it.
class WindowKeeper : public QObject {
    Q_OBJECT

public:
    // Made before the window is shown for the first time: puts it where it was, if that can be.
    // `screens` says which screens there are, each by its free part; without one, Windows is
    // asked. A test hands in its own, and lets the window come to rest sooner.
    WindowKeeper(QWindow& window, core::WindowSlot& kept, const std::function<QList<QRect>()>& screens = {},
                 int restMs = 400, QObject* parent = nullptr);

    // Shows the window as it was: filling a screen, filling the desktop, or neither.
    void show();
    void flush() { remember(); }
    void restore(const std::optional<core::WindowPlace>& place, const std::function<QList<QRect>()>& screens = {});

    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // How the window is shown now: which of the two it is, is known at once.
    void note(QWindow::Visibility now);
    // And where it is, once it has come to rest: written down with the rest.
    void remember();

    QWindow& m_window;
    core::WindowSlot& m_kept;
    // Where the window is while it is of its own size, as far as that was seen; and which of the
    // other two it is.
    core::WindowPlace m_place;
    bool m_placeKnown = false;
    QTimer m_rest;
};

} // namespace qkvm::ui
