#include <qkvm/core/wall_feed.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <QPointer>

#include <qkvm/core/server_client.h>

namespace qkvm::core {

namespace {

// A picture that is a little older than the pace is as good as a new one: another screen that looks
// at the same machine has just had one taken.
constexpr double mayBeOlderByHalf = 0.5;

// The pace of pictures of a machine that is looked at closely, where the wall's own is slower.
constexpr double closelySeconds = 0.25;

// As many pictures are on their way at once, and no more.
constexpr qsizetype picturesAtATime = 4;
// A machine whose picture could not be had is asked again after this long at the earliest; one
// whose stream is only being woken, after this long at the latest.
constexpr int failedAgainAfterMs = 3000;
constexpr int wakingAgainAfterMs = 1000;

// After something was done to a machine: milliseconds from one look to the next. About 1, 3, 6 and
// 10 seconds after, as the page has it.
constexpr std::array looksAfterMs{900, 2200, 3300, 3600};

} // namespace

WallFeed::WallFeed(WallSource& source, int statusEveryMs, QObject* parent) : QObject(parent), m_source(&source)
{
    m_statusTimer.setInterval(statusEveryMs);
    connect(&m_statusTimer, &QTimer::timeout, this, &WallFeed::askStatus);
    connect(&m_pictureTimer, &QTimer::timeout, this, &WallFeed::askPictures);
    m_lookTimer.setSingleShot(true);
    connect(&m_lookTimer, &QTimer::timeout, this, &WallFeed::lookAgain);
    m_clock.start();
}

void WallFeed::start()
{
    ++m_run;
    m_running = true;
    // Whoever starts it has just been answered by the server.
    m_inContact = true;
    m_wallAsked = false;
    m_wallAskedAnew = false;
    m_statusAsked = false;
    m_statusAskedAnew = false;
    m_pictureAsked.clear();
    m_pictureWaiting.clear();
    m_pictureDue.clear();
    m_pictured.clear();
    m_stills.clear();
    askWall();
    askStatus();
    m_statusTimer.start();
}

void WallFeed::stop()
{
    ++m_run;
    m_running = false;
    m_statusTimer.stop();
    m_pictureTimer.stop();
    m_lookTimer.stop();
    m_looksLeft = 0;
}

void WallFeed::setSource(WallSource& source)
{
    // Not under a feed that is asking: its answers would be half one wall's and half the other's.
    if (m_running) {
        return;
    }
    m_source = &source;
    // (What the place before still answers is of a run that has ended, and is ignored as such.)
    m_wallAsked = false;
    m_statusAsked = false;
    m_statusAskedAnew = false;
    m_pictureAsked.clear();
    m_pictureWaiting.clear();
    m_pictureDue.clear();
    m_pictured.clear();
    m_stills.clear();
    // What the place before named is not this one's word.
    m_everySeconds = 1;
    m_liveForm = 0;
    const bool hadMachines = !m_machines.isEmpty();
    const bool hadStatus = !m_status.isEmpty();
    m_machines.clear();
    m_status.clear();
    if (hadMachines) {
        emit wallChanged();
    }
    if (hadStatus) {
        emit statusChanged();
    }
}

void WallFeed::takeOrder(const QStringList& machines)
{
    QHash<QString, qsizetype> place;
    for (qsizetype at = 0; at < machines.size(); ++at) {
        place.insert(machines.at(at), at);
    }
    const QList<Machine> before = m_machines;
    std::ranges::stable_sort(m_machines, {},
                             [&](const Machine& machine) { return place.value(machine.id, machines.size()); });
    const bool same = std::ranges::equal(m_machines, before, {}, &Machine::id, &Machine::id);
    if (!same) {
        emit wallChanged();
    }
}

void WallFeed::lookAgainSoon()
{
    if (!m_running) {
        return;
    }
    const bool underWay = m_looksLeft > 0;
    m_looksLeft = static_cast<int>(looksAfterMs.size());
    if (!underWay) {
        m_lookTimer.start(looksAfterMs.front());
    }
}

void WallFeed::askAnew()
{
    // (What is asked here asks nothing while the feed is stopped; what is noted is forgotten
    // where it starts.)
    m_wallAskedAnew = m_wallAsked;
    askWall();
    askStatus();
}

void WallFeed::lookNow()
{
    // (Stopped, nothing is asked, and nothing is noted for later: a start asks everything.)
    if (!m_running) {
        return;
    }
    m_statusAskedAnew = m_statusAsked;
    askStatus();
}

void WallFeed::lookAgain()
{
    if (!m_running || m_looksLeft <= 0) {
        m_looksLeft = 0;
        return;
    }
    --m_looksLeft;
    askStatus();
    if (m_looksLeft > 0) {
        m_lookTimer.start(looksAfterMs.at(looksAfterMs.size() - static_cast<std::size_t>(m_looksLeft)));
    }
}

void WallFeed::setShown(const QHash<QString, int>& widths)
{
    m_shown = widths;
    // A card that has just come into sight does not wait for the next look.
    if (m_pictureTimer.isActive()) {
        askPictures();
    }
}

int WallFeed::paceMs(const QString& machine) const
{
    return std::max(1, static_cast<int>(std::lround(paceSeconds(machine) * 1000)));
}

int WallFeed::tickMs() const
{
    const double fastest = m_closely.isEmpty() ? wallSeconds() : std::min(wallSeconds(), closelySeconds);
    return std::min(std::max(1, static_cast<int>(std::lround(fastest * 1000))), wakingAgainAfterMs);
}

void WallFeed::turnsBeginAnew()
{
    m_pictureWaiting.clear();
    m_pictureDue.clear();
    if (m_pictureTimer.isActive()) {
        m_pictureTimer.start(tickMs());
        askPictures();
    }
}

double WallFeed::wallSeconds() const
{
    return m_ownEveryMs.value_or(0) > 0 ? *m_ownEveryMs / 1000.0 : m_everySeconds;
}

double WallFeed::paceSeconds(const QString& machine) const
{
    return m_closely.contains(machine) ? std::min(wallSeconds(), closelySeconds) : wallSeconds();
}

int WallFeed::serverPaceMs() const
{
    return static_cast<int>(std::lround(m_everySeconds * 1000));
}

void WallFeed::setPictureEvery(std::optional<int> everyMs)
{
    if (everyMs == m_ownEveryMs) {
        return;
    }
    m_ownEveryMs = everyMs;
    // Whoever ends a pause, or chooses another pace, is looking at the wall now.
    turnsBeginAnew();
}

void WallFeed::setLookedAtClosely(const QSet<QString>& machines)
{
    if (machines == m_closely) {
        return;
    }
    m_closely = machines;
    // And one at once: whoever has just enlarged a machine is looking at it now.
    turnsBeginAnew();
}

std::optional<int> WallFeed::stillAgeOf(const QString& machine) const
{
    const auto still = m_stills.constFind(machine);
    if (still == m_stills.constEnd() || !still->ageSeconds) {
        return std::nullopt;
    }
    // As old as it was when it came, and older by the time since.
    const qint64 since = std::max<qint64>(0, m_clock.elapsed() - still->cameAtMs) / 1000;
    return static_cast<int>(std::min<qint64>(*still->ageSeconds + since, std::numeric_limits<int>::max()));
}

void WallFeed::showAnew()
{
    m_pictured.clear();
    turnsBeginAnew();
}

// An answer is taken only while the feed is still there and still in the run that asked: the source
// may outlive the feed, and a stop or a new start makes what was asked before nobody's business.
template <typename Answer, typename Then>
std::function<void(base::Result<Answer>)> WallFeed::whileAsking(Then then)
{
    return [self = QPointer<WallFeed>(this), run = m_run, then = std::move(then)](base::Result<Answer> answer) mutable {
        if (self != nullptr && run == self->m_run) {
            then(std::move(answer));
        }
    };
}

bool WallFeed::isGone(const base::Error& error)
{
    if (error.code == ServerClient::signedOut) {
        stop();
        emit signedOut();
        return true;
    }
    if (error.code == ServerClient::certificateChanged || error.code == ServerClient::certificateUnknown) {
        stop();
        emit certificateRefused();
        return true;
    }
    if (error.code == ServerClient::otherVersion) {
        // A QKVM came back that speaks another version of its API: no wall of "contact lost"
        // for as long as it is left standing, since one of the two has to be updated first.
        stop();
        emit otherVersion(QString::fromStdString(error.message));
        return true;
    }
    return false;
}

void WallFeed::noteContact(bool inContact)
{
    if (inContact != m_inContact) {
        m_inContact = inContact;
        emit contactChanged();
    }
}

void WallFeed::askWall()
{
    if (!m_running || m_wallAsked) {
        return;
    }
    m_wallAsked = true;
    m_source->askWall(whileAsking<Wall>([this](base::Result<Wall> answer) {
        m_wallAsked = false;
        if (!answer) {
            if (!isGone(answer.error())) {
                noteContact(false);
            }
            return;
        }
        m_machines = answer.value().machines;
        m_everySeconds = answer.value().everySeconds;
        m_liveForm = answer.value().liveForm;
        m_pictureTimer.start(tickMs());
        emit wallChanged();
        askPictures();
        if (std::exchange(m_wallAskedAnew, false)) {
            askWall();
        }
    }));
}

void WallFeed::askStatus()
{
    if (!m_running || m_statusAsked) {
        return;
    }
    m_statusAsked = true;
    m_source->askStatus(whileAsking<QHash<QString, Status>>([this](base::Result<QHash<QString, Status>> answer) {
        m_statusAsked = false;
        // The place said meanwhile that a machine has come to something: this answer may have
        // been made before that, so it is asked once more, whatever this one says. (Not of a
        // place that does not answer: that one is asked again at the feed's own pace.)
        const bool anew = std::exchange(m_statusAskedAnew, false);
        if (!answer) {
            if (!isGone(answer.error())) {
                noteContact(false);
                emit statusObserved(false);
            }
            return;
        }
        noteContact(true);
        m_status = std::move(answer).value();
        emit statusChanged();
        emit statusObserved(true);
        // Somebody added or removed a machine elsewhere: this wall follows.
        const bool sameMachines =
            m_status.size() == m_machines.size() &&
            std::ranges::all_of(m_machines, [&](const Machine& machine) { return m_status.contains(machine.id); });
        if (!sameMachines) {
            askWall();
        }
        if (anew) {
            askStatus();
        }
    }));
}

void WallFeed::askPictures()
{
    if (!m_running) {
        return;
    }
    // Paused: nothing new for the wall. A card that has no picture yet gets its first; and a
    // machine that is looked at closely is another matter.
    const bool paused = m_ownEveryMs == 0;
    // (A turn that comes before the next look is taken now: a moment early is not a look missed.)
    const qint64 by = m_clock.elapsed() + tickMs() / 2;
    for (const Machine& machine : std::as_const(m_machines)) {
        const QString& id = machine.id;
        // Not looked at, or without a picture to give; the last one asked for is still out, or it
        // waits for a question that is free already; or its turn has not come.
        // (Never of a PC with Intel AMT in health only: hasAScreenToShow.)
        const auto said = m_status.constFind(id);
        const bool hasOne = hasAScreenToShow(machine, said == m_status.constEnd() ? nullptr : &*said);
        if (!m_shown.contains(id) || !hasOne || m_pictureAsked.contains(id) || m_pictureWaiting.contains(id) ||
            (paused && !m_closely.contains(id) && m_pictured.contains(id)) || m_pictureDue.value(id) > by) {
            continue;
        }
        m_pictureWaiting.append(id);
    }
    sendPictures();
}

void WallFeed::sendPictures()
{
    while (m_running && m_pictureAsked.size() < picturesAtATime && !m_pictureWaiting.isEmpty()) {
        const QString id = m_pictureWaiting.takeFirst();
        const auto shown = m_shown.constFind(id);
        // Its card went out of sight while it waited.
        if (shown == m_shown.constEnd()) {
            continue;
        }
        m_pictureAsked.insert(id);
        const qint64 sent = m_clock.elapsed();
        m_source->askSnapshot(
            id, *shown, paceSeconds(id) * mayBeOlderByHalf,
            [self = QPointer<WallFeed>(this), run = m_run, id, sent](const base::Result<Snapshot>& answer) {
                // (As whileAsking has it: only while the feed is there, and in this run.)
                if (self != nullptr && run == self->m_run) {
                    self->pictureAnswered(id, sent, answer);
                }
            });
    }
}

void WallFeed::pictureAnswered(const QString& machine, qint64 sent, const base::Result<Snapshot>& answer)
{
    m_pictureAsked.remove(machine);
    if (!answer && isGone(answer.error())) {
        return;
    }
    // At the pace of now: another may have been chosen while this one was on its way.
    const int pace = paceMs(machine);
    // When its next turn comes, counted from when the question went out, as the page counts it. A
    // stream that is only being woken has pictures within a second; anything else that fails is
    // asked less often than a machine that delivers. Written down before anybody is told of the
    // answer: whoever is told may say there and then what is shown, and that is not this
    // machine's next turn.
    const bool waking = !answer && answer.error().code == "no_stream";
    const int again = answer ? pace : waking ? std::min(pace, wakingAgainAfterMs) : std::max(pace, failedAgainAfterMs);
    m_pictureDue.insert(machine, sent + again);
    if (answer) {
        m_pictured.insert(machine);
        // A still kept between sessions, and how old it was when it came (stillAgeOf); a picture
        // that is none takes its place.
        if (answer.value().still) {
            m_stills.insert(machine, Still{answer.value().ageSeconds, m_clock.elapsed()});
        } else {
            m_stills.remove(machine);
        }
        emit pictureCame(machine, answer.value().picture, answer.value().signal);
    } else {
        emit pictureFailed(machine, QString::fromStdString(answer.error().code),
                           QString::fromStdString(answer.error().message));
    }
    sendPictures();
}

} // namespace qkvm::core
