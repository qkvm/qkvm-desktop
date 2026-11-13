#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>

#include <qkvm/core/local_wall.h>
#include <qkvm/core/secrets.h>
#include <qkvm/core/settings.h>

#include "actions_view.h"
#include "edit_kvm_view.h"
#include "files_view.h"
#include "live_picture.h"
#include "merged_wall.h"
#include "pc_source.h"
#include "server_source.h"
#include "session_view.h"
#include "shared_live.h"
#include "wall_screen.h"
#include "wall_view.h"

namespace qkvm::ui {

class MachineControl;
class VideoHost;
class AppLock;
class AlertsView;
class QuickAccess;

// One foreground conversation: this PC's local wall or one QKVM server, with its
// SessionView and MergedWall. The application coordinator makes independent sessions
// for windows, sharing local devices, credentials, settings, app lock, quick access
// and the pending-action registry. Each window owns its connection and selected source.
// The coordinator owns background monitoring; only one foreground session per address
// publishes incident observations, with a status-only monitor when no window shows it.
// Display-link restrictions still apply to the whole application.
//
// On the interface thread. The views, the settings and the secrets outlive it.
class ServerSession : public QObject {
    Q_OBJECT

public:
    // `wall` is the program's own, whether a window shows it or not: the machines as the server
    // gives them, for whoever goes by them without a window (the program's icon, its messages),
    // and the pace the pictures are asked for at. How the machines are is asked as often as the
    // page asks it; a test asks more often.
    ServerSession(SessionView& view, WallView& wall, core::Settings& settings, core::SecretStore& secrets,
                  int statusEveryMs = 4000, QObject* parent = nullptr);
    ~ServerSession() override;
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;
    ServerSession(ServerSession&&) = delete;
    ServerSession& operator=(ServerSession&&) = delete;

    // The id of the one QKVM server, as its machines' keys on the wall begin with it, until servers
    // are kept each with an id of its own.
    static QString serverId();

    // A window shows the wall from now on: its wall (the program's own, where the program has the
    // one window and no other wall), what is done to machines from it, and where its pictures go:
    // the window's engine, or nowhere (a window drawn without one). Until it is taken away again,
    // which is before any of the three ends; a wall that was taken away says nothing to the
    // session that is acted on, and is not given to it a second time.
    void add(WallView& wall, ActionsView& actions, VideoHost* host);
    void remove(WallView& wall);

    // This PC's own wall, where the program has one: KVMs that were added in the program, which
    // it speaks to itself. The session shows it without a server and without anybody signed in,
    // when the screens ask for it and at the start where it was shown last (or no server ever
    // was). Once, before begin(): a second wall is not taken. The wall outlives the session.
    void useOwnWall(core::LocalWall& wall);

    // What the program does when it starts: goes to the wall that was shown last, which is this
    // PC's own or that of the server that was used last, if there is one.
    void begin();
    // A separate foreground conversation for one window; monitoring remains coordinated here.
    std::unique_ptr<ServerSession> makeWindow(SessionView& view, WallView& wall, core::WindowSlot& kept);
    SessionView& view() { return m_view; }
    AppLock* appLock() const { return m_appLock; }
    void beginWindow();
    void selectSource(const QString& source);
    QString selectedSource() const;
    QString selectedScope() const { return onOwnWall() ? QStringLiteral("pc") : m_serverScope; }
    bool mayChooseSource() const;
    // Whether an action sent to a machine from any window (or virtual media being changed for one) has not been
    // answered yet: the list of what is under way that the windows write into holds something (this session's own
    // wall's, where the windows are its; the one shared with each window's own session, where they have their own).
    bool actionsUnderWay() const { return !m_sharedActions.isEmpty() || m_merged.actionsUnderWay(); }
    void useAppLock(AppLock& lock);
    // After useOwnWall and before begin: observations from every saved source.
    void useAlerts(AlertsView& alerts);
    void useQuickAccess(QuickAccess& shortcuts);
    // A notification's identity is checked against the account and fresh inventory before opening.
    void openIncident(const QString& scope, const QString& machine, WallView& wall);
    void stopForReset();

    // What time it is, where a test has a clock of its own: the wall says since when the server
    // does not answer.
    void tellTheTimeWith(std::function<QDateTime()> clock);
    // How a machine's own console is opened in a browser (see MachineActions): the program hands in
    // Windows' way, a test its own.
    void openPagesWith(std::function<bool(const QUrl&)> opener);
    // The wall as a file ("Export…", "Import…"): what this view's boxes ask is asked of the
    // server from now on, for whoever is at the wall, and it is told what came of it; nothing
    // while the program only shows. One view for each window; one that ends is served no longer.
    void serve(FilesView& files);
    // The box that changes a KVM of this PC's own wall, or takes it off: it may, from now on,
    // only while the program does not only show (EditKvmView::setMayChange). One for each
    // window.
    void serve(EditKvmView& editing);
    // How long a window waits before it asks again for a machine live, in the windows that are
    // added from now on: a test waits less.
    void retryLiveAt(LivePicture::Pace pace) { m_merged.retryLiveAt(pace); }
    // Keyboard and mouse of the machine a window shows large go through this from now on
    // (control.h): it is handed the Live that window's picture holds, at a server's wall and at
    // this PC's own, whenever that is another one or none; and is told whether whoever sits here
    // may act on machines, which nobody may at a wall that is only shown. For a wall that was
    // added, until it is taken away: before that, what the machine holds is let go.
    void drive(WallView& wall, MachineControl& control);

signals:
    // Nobody is at the wall any more: for what has something under way that is then nobody's.
    void wallWasLeft();
    void incidentDetailsAsked();
    void incidentUnavailable();

private:
    // Whether this PC's own wall is the one that is shown.
    bool onOwnWall() const;
    // Whether somebody is at the QKVM server's wall, which is not the one shown where this PC's
    // own is.
    bool isAtAServersWall() const;
    // To this PC's own wall, and away from it.
    void enterOwnWall();
    void leaveOwnWall();
    void rememberWhichWall(bool ownWall);
    // A server is named: not from this PC's own wall, which is left first.
    void connectTo(const QString& address);
    // What the screens are shown: this PC's own wall, or where the program is with the server.
    void show();
    // The server's machines are the windows' wall's from now on, after this PC's where it has one:
    // before anything is asked of the server. (Once.)
    void takeTheServer();
    void refreshMonitoring();
    void finishOpeningIncident();
    void stopBackgroundAt(const QString& address);
    void rememberAlertAddress(const QString& scope, const QString& address);
    std::optional<core::MachineReference> shortcutOf(const QString& key) const;
    // The size a machine of the wall is shown at large, kept on this PC (b10): by the same reference as
    // its favorite's, of this PC's own machines and of the server's for the account signed in there.
    std::optional<core::MachineReference> scaleReferenceOf(const QString& key) const;
    QString scaleKeptFor(const QString& key) const;
    void keepScale(const QString& key, const QString& choice);
    void showFavorites(WallView& wall);
    void showSources();
    void rememberSource(const QString& source);
    ServerSession* observerAt(const QString& address) const;
    bool observesServer() const;
    bool clearScreens();

    SessionView& m_view;
    core::Settings& m_settings;
    core::SecretStore& m_secrets;
    // How often the machines are looked at, on either wall.
    int m_statusEveryMs;
    // Whether this PC is a wall screen. Before the server, which says so, and the windows' wall,
    // which goes by it.
    WallScreen m_screen;
    // The QKVM server, its client and the feed that keeps its wall; and whether it is on the
    // windows' wall yet.
    ServerSource m_server;
    bool m_serverTaken = false;
    // This PC's own wall, where there is one, with the feed that keeps it: it knows whether it is
    // the wall that is shown.
    std::unique_ptr<PcSource> m_pc;
    // The wall as the windows have it, with both sources on it. Made last: it is the only one that
    // hears either feed's word of the wall, its machines and their pictures, and it ends first,
    // with every window's streams and keyboard and mouse.
    MergedWall m_merged;
    AppLock* m_appLock = nullptr;
    QPointer<AlertsView> m_alerts;
    QPointer<QuickAccess> m_shortcuts;
    QString m_serverScope;
    // The same of the server whose wall this window is at, for the sizes its machines are shown at
    // large (b10), whether or not alerts are watched.
    QString m_scaleScope;
    QString m_foregroundAddress;
    struct Background {
        std::unique_ptr<ServerSource> source;
        QString scope;
        QString placeholder;
    };
    std::vector<std::unique_ptr<Background>> m_background;
    QHash<QString, QString> m_alertAddresses;
    QPointer<WallView> m_openWall;
    QString m_openScope;
    QString m_openMachine;
    bool m_openConfirmed = false;
    bool m_monitoringStopped = false;
    core::LocalWall* m_localWall = nullptr;
    core::WindowSlot* m_slot = nullptr;
    ServerSession* m_coordinator = nullptr;
    QList<QPointer<ServerSession>> m_windowSessions;
    QString m_selectedSource;
    bool m_windowStarted = false;
    bool m_windowMode = false;
    std::function<bool(const QUrl&)> m_opener;
    UnderWay m_sharedActions;
    SharedLive m_sharedLive;
    QList<QPointer<WallView>> m_walls;
};

} // namespace qkvm::ui
