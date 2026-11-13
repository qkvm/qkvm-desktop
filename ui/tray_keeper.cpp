#include "tray_keeper.h"

#include <QCoreApplication>

#include "quick_access.h"
#include "window_keeper.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Tray", text);
}

} // namespace

TrayKeeper::TrayKeeper(TrayIcon& tray, SessionView& session, WallView& wall, core::Settings& settings, QObject* parent)
    : QObject(parent), m_tray(tray), m_session(session), m_wall(wall), m_settings(settings)
{
    connect(&m_session, &SessionView::changed, this, &TrayKeeper::say);
    connect(m_wall.machines(), &WallModel::countsChanged, this, &TrayKeeper::say);
    connect(&m_tray, &TrayIcon::opened, this, &TrayKeeper::bringAllBack);
    connect(&m_tray, &TrayIcon::chosen, this, [this](int row) {
        if (row == Open) {
            bringAllBack();
        } else if (row == Page) {
            (m_followedSession != nullptr ? *m_followedSession : m_session).openPage();
        } else if (row == Quit) {
            m_ending = true;
            emit quitAsked();
        } else if (row == ClearRecent && m_shortcuts != nullptr) {
            m_shortcuts->clearRecent();
        } else if (row == ClearFavorites && m_shortcuts != nullptr) {
            m_shortcuts->clearFavorites();
        }
    });
    say();
}

void TrayKeeper::useQuickAccess(QuickAccess& shortcuts)
{
    m_shortcuts = &shortcuts;
    connect(&shortcuts, &QuickAccess::changed, this, &TrayKeeper::say);
    say();
}

void TrayKeeper::keep(QWindow& window)
{
    if (!m_windows.contains(&window)) {
        m_windows.append(&window);
        window.installEventFilter(this);
    }
}

void TrayKeeper::letGo(QWindow& window)
{
    m_windows.removeAll(&window);
}

void TrayKeeper::bringAllBack()
{
    // The first one last: it is the one in front, and has the keyboard.
    for (auto window = m_windows.crbegin(); window != m_windows.crend(); ++window) {
        bringBack(**window);
    }
}

void TrayKeeper::say()
{
    const auto& session = m_followedSession != nullptr ? *m_followedSession : m_session;
    auto& wall = m_followedWall != nullptr ? *m_followedWall : m_wall;
    // The page is a QKVM server's: at this PC's own wall there is none.
    const bool atAServersWall = session.stage() == "wall"_L1 && !session.ownWall();
    {
        QList<TrayIcon::Row> rows{{Open, words("Open qKVM")}};
        if (atAServersWall) {
            rows.append({Page, words("Open the qKVM page")});
        }
        if (m_shortcuts != nullptr && m_shortcuts->allowed()) {
            int next = 100;
            const auto submenu = [&next](const QString& title, const QList<MachineChoice>& choices,
                                         const QString& empty, int clear, const QString& clearText) {
                TrayIcon::Row menu;
                menu.id = next++;
                menu.text = title;
                for (const auto& choice : choices) {
                    TrayIcon::Row row;
                    row.id = next++;
                    row.text = choice.name.simplified().left(60) + u" - "_s + choice.source.simplified().left(60);
                    row.scope = choice.reference.scope;
                    row.machine = choice.reference.machine;
                    menu.children.append(row);
                }
                if (choices.isEmpty()) {
                    TrayIcon::Row row;
                    row.id = next++;
                    row.text = empty;
                    row.enabled = false;
                    menu.children.append(row);
                }
                menu.children.append(TrayIcon::Row{});
                menu.children.append({clear, clearText});
                return menu;
            };
            rows.append(TrayIcon::Row{});
            rows.append(submenu(words("Favorites"), m_shortcuts->favorites(), words("No available favorites"),
                                ClearFavorites, words("Clear favorites")));
            rows.append(submenu(words("Recent machines"), m_shortcuts->recent(), words("No recent machines"),
                                ClearRecent, words("Clear recent machines")));
        }
        rows.append({0, QString()});
        rows.append({Quit, words("Quit")});
        if (rows != m_menu) {
            m_menu = rows;
            m_tray.setMenu(rows);
        }
    }
    const WallModel& machines = *wall.machines();
    const TrayState state = trayStateOf(session.stage(), session.inContact(), machines.all(), machines.online(),
                                        machines.noSignal(), machines.attention());
    if (state != m_said) {
        m_said = state;
        m_tray.show(state.dot, state.says);
    }
}

void TrayKeeper::follow(SessionView& session, WallView& wall)
{
    if (m_followedSession == &session && m_followedWall == &wall)
        return;
    for (const auto& connection : m_followedConnections)
        disconnect(connection);
    m_followedSession = &session;
    m_followedWall = &wall;
    m_followedConnections = {connect(&session, &SessionView::changed, this, &TrayKeeper::say),
                             connect(wall.machines(), &WallModel::countsChanged, this, &TrayKeeper::say),
                             connect(&session, &QObject::destroyed, this, [this] {
                                 m_followedSession = nullptr;
                                 m_followedWall = nullptr;
                                 say();
                             })};
    say();
}

bool TrayKeeper::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Close && !m_ending && m_windows.size() == 1 && watched == m_windows.constFirst() &&
        m_tray.isThere() && m_settings.remembered().staysInTray) {
        // The program's last window: out of sight, and not closed. The program goes on behind
        // its icon.
        event->ignore();
        m_windows.constFirst()->hide();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace qkvm::ui
