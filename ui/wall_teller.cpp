#include "wall_teller.h"

#include <utility>

#include <QAbstractItemModel>
#include <QCoreApplication>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("News", text);
}

} // namespace

WallTeller::WallTeller(TrayIcon& tray, SessionView& session, WallView& wall, core::Settings& settings, int gatherMs,
                       QObject* parent)
    : QObject(parent), m_tray(tray), m_session(session), m_wall(wall), m_settings(settings)
{
    m_gather.setSingleShot(true);
    m_gather.setInterval(gatherMs);
    connect(&m_gather, &QTimer::timeout, this, &WallTeller::tell);
    const WallModel* machines = m_wall.machines();
    connect(machines, &QAbstractItemModel::dataChanged, this, &WallTeller::lookAtTheWall);
    connect(machines, &QAbstractItemModel::modelReset, this, &WallTeller::lookAtTheWall);
    connect(&m_session, &SessionView::changed, this, &WallTeller::lookAtTheSession);
    lookAtTheSession();
}

void WallTeller::lookAtTheSession()
{
    if (m_session.stage() != "wall"_L1) {
        // Nobody is signed in: there is no wall to tell of, and what was gathered of the last
        // one is nobody's news.
        m_atTheWall = false;
        m_look.clear();
        m_news = {};
        m_toldSilent = false;
        return;
    }
    if (!m_atTheWall) {
        // Somebody has just signed in. Whatever the wall holds at this moment is not this
        // QKVM's word yet: its first fill comes after, and is what later looks are held against.
        m_atTheWall = true;
        m_answered = m_session.inContact();
        return;
    }
    if (m_session.inContact() != m_answered) {
        m_answered = m_session.inContact();
        // Told with the rest, after a moment: a QKVM that is silent for a second is no news.
        m_gather.start();
    }
    lookAtTheWall();
}

void WallTeller::lookAtTheWall()
{
    // While QKVM does not answer, the wall is as it was, and says nothing new of any machine.
    if (!m_atTheWall || !m_session.inContact()) {
        return;
    }
    const WallModel& machines = *m_wall.machines();
    add(m_news, newsBetween(m_look, machines));
    remember(m_look, machines);
    if (!m_news.isEmpty() && !m_gather.isActive()) {
        m_gather.start();
    }
}

void WallTeller::tell()
{
    const WallNews news = std::exchange(m_news, {});
    // Whoever can see the wall sees it there; and whoever wants no messages gets none. What
    // happened meanwhile is not kept for later: it would be old news by then.
    const bool wanted = m_wall.isUnseen() && m_settings.remembered().tellsNews;
    if (!m_answered) {
        if (wanted) {
            m_toldSilent = true;
            m_tray.tell(words("Lost contact with qKVM"), words("Trying again. The wall is as it was."), true);
        }
        return;
    }
    const std::optional<Message> message = messageOf(news);
    if (m_toldSilent) {
        // Whoever was told that it is silent is told that it answers again, wanted or not by
        // now, and with it what became of the machines meanwhile.
        m_toldSilent = false;
        m_tray.tell(words("qKVM answers again"), message ? message->title : words("The wall is up to date."),
                    message && message->isWarning);
        return;
    }
    if (wanted && message) {
        m_tray.tell(message->title, message->text, message->isWarning);
    }
}

} // namespace qkvm::ui
