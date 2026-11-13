#pragma once

#include <optional>

#include <QEvent>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QWindow>

#include <qkvm/core/settings.h>

#include "session_view.h"
#include "tray.h"
#include "wall_view.h"

namespace qkvm::ui {
class QuickAccess;

// Keeps the program's icon in the notification area: it says how the wall is (a dot beside the
// mark, and the bar's counts for a pointer that rests on it), a click on it brings the program's
// windows to the front, and its menu opens them, opens the QKVM's page and ends the program.
//
// And, where whoever sits at the PC chose that in the settings, the program goes on behind the
// icon when its last window is closed: that window is then put out of sight and not closed, and
// the icon brings it back. (A window that is closed while another is open is closed.) Only while
// the icon is there, which is asked of it at that moment: without it nothing would bring the
// window back, and closing closes. "Quit" in the icon's menu always ends the program.
//
// A window is kept before whatever else watches it being closed (WindowKeeper), which is then
// asked first and still hears of it. On the interface thread; what it is given outlives it, and a
// window is let go of before it ends.
class TrayKeeper : public QObject {
    Q_OBJECT

public:
    // The rows of the icon's menu, by what they do.
    enum Row { Open = 1, Page, Quit, ClearRecent, ClearFavorites };

    // `wall` is the program's own: the machines as the server gives them, whatever window shows them.
    TrayKeeper(TrayIcon& tray, SessionView& session, WallView& wall, core::Settings& settings,
               QObject* parent = nullptr);

    // A window of the program's, from now on; and no longer.
    void keep(QWindow& window);
    void letGo(QWindow& window);
    // Every window back into sight and to the front, as it was: for a click on the icon, and for
    // whoever started the program a second time.
    void bringAllBack();
    void useQuickAccess(QuickAccess& shortcuts);
    // The page command and status describe the last active window's source.
    void follow(SessionView& session, WallView& wall);

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // "Quit" was chosen: the program is to end, whether its window is in sight or not.
    void quitAsked();

private:
    // Tells the icon how the wall is, where that is other than what it was told last.
    void say();

    QList<QWindow*> m_windows;
    TrayIcon& m_tray;
    SessionView& m_session;
    WallView& m_wall;
    core::Settings& m_settings;
    // What the icon was told last.
    std::optional<TrayState> m_said;
    // The program is ending: its last window is closed like any other.
    bool m_ending = false;
    QPointer<QuickAccess> m_shortcuts;
    QList<TrayIcon::Row> m_menu;
    QPointer<SessionView> m_followedSession;
    QPointer<WallView> m_followedWall;
    QList<QMetaObject::Connection> m_followedConnections;
};

} // namespace qkvm::ui
