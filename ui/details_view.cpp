#include "details_view.h"

#include <algorithm>
#include <cmath>

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QImageReader>
#include <QLocale>
#include <QThreadPool>

using namespace Qt::StringLiterals;

namespace qkvm::ui {
namespace {
QString date(double seconds)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds)).toLocalTime(),
                              QLocale::ShortFormat);
}

} // namespace

QString DetailsView::stateWord(const QString& state)
{
    // (The page's three dots are the one sign for them here, as on the cards and in Words.qml.)
    static const QHash<QString, const char*> words{
        {u"connecting"_s, "Connecting…"}, {u"online"_s, "Online"},     {u"no_signal"_s, "No signal"},
        {u"offline"_s, "Offline"},        {u"auth"_s, "Login failed"}, {u"error"_s, "Problem"},
        {u"busy"_s, "In use elsewhere"},
    };
    const auto found = words.constFind(state);
    return found == words.constEnd() ? state : tr(*found);
}

namespace {

// An alert of a machine's screen watch in the page's words (app.js, watchText): what went off, or that
// it is over. A watch this program does not know (one added since) in the server's own word, with the
// sentence it wrote of it where it wrote one (README, "Clients and versions").
QString watchText(const core::ActivityEntry& entry)
{
    if (entry.watch != u"no_signal"_s && entry.watch != u"unchanged"_s) {
        return entry.said.isEmpty() ? entry.watch : entry.watch + u" · "_s + entry.said;
    }
    const QString minutes = entry.minutes ? (*entry.minutes == 1 ? DetailsView::tr("1 minute")
                                                                 : DetailsView::tr("%1 minutes").arg(*entry.minutes))
                                          : QString();
    if (entry.watch == u"no_signal"_s) {
        return entry.back          ? DetailsView::tr("Back to normal: a picture again")
               : minutes.isEmpty() ? DetailsView::tr("No signal")
                                   : DetailsView::tr("No signal for %1").arg(minutes);
    }
    return entry.back          ? DetailsView::tr("Back to normal: the screen changes again")
           : minutes.isEmpty() ? DetailsView::tr("Screen unchanged")
                               : DetailsView::tr("Screen unchanged for %1").arg(minutes);
}

// An alert's line, as the page's alertWords: a watch's, or a change of state.
QString alertWords(const core::ActivityEntry& entry)
{
    return entry.watch.isEmpty()
               ? DetailsView::tr("%1 to %2").arg(DetailsView::stateWord(entry.was), DetailsView::stateWord(entry.state))
               : watchText(entry);
}

} // namespace

DetailsView::DetailsView(QObject* parent) : QObject(parent)
{
    m_refresh.setSingleShot(true);
    connect(&m_refresh, &QTimer::timeout, this, &DetailsView::refresh);
}

QByteArray DetailsView::identity(const core::ServerClient* client)
{
    return client == nullptr ? QByteArray()
                             : QCryptographicHash::hash(client->address().toEncoded() + '\n' + client->session(),
                                                        QCryptographicHash::Sha256);
}

void DetailsView::clear()
{
    m_summary.clear();
    m_graphs.clear();
    m_activity.clear();
    m_pictures.clear();
    // (A picture is the machine's and the account's that it was looked at for.)
    closePicture();
    m_historyProblem.clear();
    m_graphProblem.clear();
    m_activityProblem.clear();
    m_graphNote.clear();
    m_activityNote.clear();
}

void DetailsView::bind(core::ServerClient* client, const QString& machine, const QString& scope, bool administrator,
                       bool readable)
{
    const auto partition = identity(client);
    if (m_own == nullptr && m_client == client && m_machine == machine && m_scope == scope && m_identity == partition &&
        m_administrator == administrator && m_readable == readable) {
        return;
    }
    disconnect(m_ownSaid);
    disconnect(m_ownRead);
    m_own = nullptr;
    ++m_epoch;
    m_refresh.stop();
    m_client = client;
    m_machine = machine;
    m_scope = scope;
    m_identity = partition;
    m_administrator = administrator;
    m_readable = readable;
    m_failures = 0;
    m_again = false;
    clear();
    emit changed();
    if (m_opened) {
        refresh();
    }
}

void DetailsView::bindOwn(core::ScreenWatch* screens, const QString& machine)
{
    if (screens == nullptr) {
        bind(nullptr, {}, {}, false, false);
        return;
    }
    if (m_own == screens && m_client == nullptr && m_machine == machine) {
        return;
    }
    disconnect(m_ownSaid);
    disconnect(m_ownRead);
    ++m_epoch;
    m_refresh.stop();
    m_client = nullptr;
    m_own = screens;
    m_machine = machine;
    m_scope = u"pc"_s;
    m_identity.clear();
    m_administrator = true;
    m_readable = true;
    m_failures = 0;
    m_again = false;
    clear();
    // (What they tell of it from now on is shown as it comes.)
    m_ownSaid = connect(screens, &core::ScreenWatch::alerted, this, [this](const core::ScreenWatch::Event& event) {
        if (event.machine == m_machine && m_opened) {
            showOwn();
        }
    });
    // (And what was told before this run, once it is read back: in a thread of the watches' own, review 4's LOW 5.)
    m_ownRead = connect(screens, &core::ScreenWatch::readBack, this, [this] {
        if (m_opened) {
            showOwn();
        }
    });
    emit changed();
    if (m_opened) {
        refresh();
    }
}

void DetailsView::showOwn()
{
    m_activity.clear();
    m_pictures.clear();
    m_activityNote.clear();
    core::DeviceActivity activity;
    if (m_own != nullptr) {
        for (const core::ScreenWatch::Event& event : m_own->told(m_machine)) {
            core::ActivityEntry entry;
            entry.at = static_cast<double>(event.at.toSecsSinceEpoch());
            entry.event = u"alert"_s;
            entry.watch = event.watch;
            entry.minutes = event.minutes;
            entry.back = event.back;
            entry.picture = event.stamp;
            if (event.pictureAt.isValid()) {
                entry.pictureAt = static_cast<double>(event.pictureAt.toSecsSinceEpoch());
            }
            activity.entries.append(entry);
        }
    }
    show(activity);
    if (activity.entries.isEmpty()) {
        m_activityNote = tr("No screen alerts of it yet. Its screen watches are set in its Change box.");
    }
    emit changed();
}

void DetailsView::setOpened(bool opened)
{
    if (m_opened == opened) {
        return;
    }
    m_opened = opened;
    ++m_epoch;
    m_refresh.stop();
    m_again = false;
    clear();
    emit changed();
    if (opened) {
        refresh();
    }
}

bool DetailsView::accepts(quint64 epoch) const
{
    return epoch == m_epoch && m_opened && available() && m_identity == identity(m_client);
}

bool DetailsView::denied(const base::Error& error)
{
    if (error.code != "not_allowed" && error.code != "not_found" && error.code != "not_signed_in" &&
        error.code != "unauthorized" && error.code != "auth_required" && error.code != "totp_required" &&
        error.code != "suspended" && error.code != "forbidden") {
        return false;
    }
    ++m_epoch;
    m_again = false;
    m_refresh.stop();
    clear();
    m_historyProblem = QString::fromStdString(error.message);
    return true;
}

void DetailsView::refresh()
{
    if (!m_opened || !available()) {
        return;
    }
    if (m_own != nullptr) {
        showOwn();
        return;
    }
    if (m_pending != 0) {
        m_again = true;
        return;
    }
    m_refresh.stop();
    m_again = false;
    m_retry = false;
    m_historyProblem.clear();
    m_graphProblem.clear();
    m_activityProblem.clear();
    const quint64 epoch = ++m_epoch;
    m_batch = epoch;
    m_pending = administrator() ? 2 : 1;
    // Retargeting waits for the current bounded batch rather than queuing a new
    // pair on every click. Destruction of its client settles the whole batch.
    m_batchGone = connect(m_client, &QObject::destroyed, this, [this, epoch] {
        if (m_batch != epoch) {
            return;
        }
        m_pending = 0;
        if (m_epoch == epoch) {
            ++m_epoch;
            clear();
        }
        emit changed();
        if (m_again && m_opened && available()) {
            QTimer::singleShot(0, this, &DetailsView::refresh);
        }
    });
    emit changed();
    const QPointer<DetailsView> self(this);
    m_client->askHistory(m_machine, [self, epoch](const base::Result<core::HistorySummary>& result) {
        if (!self) {
            return;
        }
        if (self->accepts(epoch)) {
            if (result) {
                self->show(result.value());
                self->m_retry = self->m_retry || !result.value().share.has_value();
                if (!result.value().sensors.isEmpty()) {
                    ++self->m_pending;
                    self->m_client->askHistorySamples(
                        self->m_machine, [self, epoch](const base::Result<core::HistorySamples>& samples) {
                            if (!self) {
                                return;
                            }
                            if (self->accepts(epoch)) {
                                self->m_graphs.clear();
                                self->m_graphNote.clear();
                                if (samples) {
                                    self->show(samples.value());
                                } else if (!self->denied(samples.error())) {
                                    self->m_graphProblem = QString::fromStdString(samples.error().message);
                                    self->m_retry = true;
                                }
                            }
                            self->finish(epoch);
                        });
                } else {
                    self->m_graphs.clear();
                    self->m_graphNote.clear();
                }
            } else if (!self->denied(result.error())) {
                self->m_summary.clear();
                self->m_graphs.clear();
                self->m_historyProblem = QString::fromStdString(result.error().message);
                self->m_retry = true;
            }
        }
        self->finish(epoch);
    });
    if (administrator()) {
        m_client->askDeviceActivity(m_machine, [self, epoch](const base::Result<core::DeviceActivity>& result) {
            if (!self) {
                return;
            }
            if (self->accepts(epoch) && self->administrator()) {
                self->m_activity.clear();
                self->m_pictures.clear();
                self->m_activityNote.clear();
                if (result) {
                    self->show(result.value());
                } else if (!self->denied(result.error())) {
                    self->m_activityProblem = QString::fromStdString(result.error().message);
                    self->m_retry = true;
                }
            }
            self->finish(epoch);
        });
    }
}

void DetailsView::finish(quint64 epoch)
{
    if (epoch != m_batch || m_pending == 0) {
        return;
    }
    --m_pending;
    if (m_pending == 0) {
        disconnect(m_batchGone);
        if (m_again && m_opened && available()) {
            QTimer::singleShot(0, this, &DetailsView::refresh);
        } else if (accepts(epoch)) {
            m_failures = m_retry ? std::min(5, m_failures + 1) : 0;
            m_refresh.start(m_retry ? std::min(60000, 5000 * (1 << (m_failures - 1))) : 60000);
        }
    }
    emit changed();
}

void DetailsView::show(const core::HistorySummary& summary)
{
    QString online;
    if (summary.share) {
        const double percent = *summary.share >= 1 ? 100 : std::min(99.9, *summary.share * 100);
        online = QString::number(percent, 'f', std::floor(percent) == percent ? 0 : 1) + u" %"_s;
    }
    const QString watched = summary.watched < 60     ? tr("%1 s").arg(qRound64(summary.watched))
                            : summary.watched < 3600 ? tr("%1 min").arg(qRound64(summary.watched / 60))
                                                     : tr("%1 h").arg(QString::number(summary.watched / 3600, 'f', 1));
    m_summary = {{u"known"_s, summary.share.has_value()},
                 {u"online"_s, online},
                 {u"watched"_s, tr("%1 watched this week").arg(watched)},
                 {u"lastSeen"_s, summary.lastSeen ? date(*summary.lastSeen) : tr("never")},
                 {u"firstSeen"_s, summary.firstSeen ? date(*summary.firstSeen) : tr("never")},
                 {u"since"_s, date(summary.since)},
                 {u"until"_s, date(summary.until)},
                 {u"changes"_s, summary.changes},
                 {u"events"_s, summary.events}};
}

void DetailsView::show(const core::HistorySamples& samples)
{
    bool omitted = samples.more;
    for (const auto& unit : {u"C"_s, u"RPM"_s, u"%"_s, u"W"_s}) {
        QList<core::HistorySeries> lines;
        for (const auto& line : samples.series) {
            if (line.unit == unit && !line.points.isEmpty()) {
                lines.append(line);
            }
        }
        std::ranges::sort(lines, [](const auto& first, const auto& second) {
            return first.points.constLast().value > second.points.constLast().value;
        });
        if (lines.isEmpty()) {
            continue;
        }
        if (lines.size() > 8) {
            omitted = true;
            lines.resize(8);
        }
        QVariantList drawn;
        double low = lines.constFirst().points.constFirst().low;
        double high = lines.constFirst().points.constFirst().high;
        for (const auto& line : lines) {
            QVariantList points;
            for (const auto& point : line.points) {
                points.append(QVariant(QVariantList{point.at, point.value, point.low, point.high}));
                low = std::min(low, point.low);
                high = std::max(high, point.high);
            }
            drawn.append(QVariantMap{{u"name"_s, line.sensor},
                                     {u"points"_s, points},
                                     {u"latest"_s, QString::number(line.points.constLast().value, 'f', 1)}});
        }
        const QString title = unit == u"C"_s ? tr("Temperature") : unit == u"W"_s ? tr("Power") : tr("Fans");
        m_graphs.append(QVariantMap{{u"title"_s, title},
                                    {u"unit"_s, unit == u"C"_s ? u"°C"_s : unit},
                                    {u"since"_s, samples.since},
                                    {u"until"_s, samples.until},
                                    {u"low"_s, low},
                                    {u"high"_s, high},
                                    {u"lines"_s, drawn}});
    }
    for (const auto& line : samples.series) {
        if (line.unit != u"C"_s && line.unit != u"RPM"_s && line.unit != u"%"_s && line.unit != u"W"_s) {
            omitted = true;
        }
    }
    m_graphNote =
        omitted ? tr("Some sensor history is not shown. Up to eight sensors of each supported unit are displayed.")
                : QString();
}

void DetailsView::show(const core::DeviceActivity& activity)
{
    for (const auto& entry : activity.entries) {
        if (entry.event == u"live_opened"_s || entry.event == u"live_closed"_s) {
            continue;
        }
        QString what = entry.event;
        if (entry.event == u"action"_s || entry.event == u"action_failed"_s) {
            what = entry.name.isEmpty() ? entry.action : entry.name;
            if (entry.event == u"action_failed"_s) {
                what = tr("%1 failed").arg(what);
            }
        } else if (entry.event == u"alert"_s) {
            what = tr("Alert: %1").arg(alertWords(entry));
        } else if (entry.event == u"live_control"_s) {
            if (entry.on.value_or(false)) {
                what = entry.from.isEmpty() ? tr("Keyboard and mouse taken")
                                            : tr("Keyboard and mouse taken over from %1").arg(entry.from);
            } else if (entry.why == u"taken"_s) {
                what = entry.by.isEmpty() ? tr("Keyboard and mouse taken over")
                                          : tr("Keyboard and mouse taken over by %1").arg(entry.by);
            } else {
                what = entry.why.isEmpty() || entry.why == u"closed"_s || entry.why == u"idle"_s
                           ? tr("Keyboard and mouse given back")
                           : tr("Keyboard and mouse taken away");
            }
        } else {
            what.replace(u'_', u' ');
        }
        // (An alert's picture, by the row it is of: its stamp stays here.)
        const bool pictured = entry.event == u"alert"_s && !entry.picture.isEmpty();
        m_activity.append(QVariantMap{{u"when"_s, date(entry.at)},
                                      {u"user"_s, entry.user.isEmpty() ? u"qKVM"_s : entry.user},
                                      {u"what"_s, what},
                                      {u"picture"_s, pictured},
                                      {u"row"_s, m_pictures.size()}});
        m_pictures.append(
            pictured ? PictureOf{entry.picture, alertWords(entry), entry.pictureAt ? date(*entry.pictureAt) : QString()}
                     : PictureOf{});
    }
    QStringList notes;
    if (activity.more) {
        notes.append(tr("More activity is available on the server."));
    }
    if (activity.missing > 0) {
        notes.append(tr("The server reports %1 activity entries could not be recorded.").arg(activity.missing));
    }
    if (m_activity.isEmpty()) {
        notes.append(activity.entries.isEmpty() ? tr("Nothing done to it in the activity log yet")
                                                : tr("Only live views in the latest activity entries"));
    }
    m_activityNote = notes.join(u' ');
}

QImage DetailsView::pictureFrom(const QByteArray& bytes, qint64 mostPixels)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    // What it is, by how it begins, and how large it says it is: before a pixel of it is decoded.
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QByteArray format = reader.format();
    const QSize size = reader.size();
    if ((format != "jpeg" && format != "png") || !size.isValid() || size.width() > 16384 || size.height() > 16384 ||
        qint64{size.width()} * size.height() > mostPixels) {
        return {};
    }
    QImage image = reader.read();
    return image.size() == size ? image : QImage();
}

void DetailsView::showPicture(int row)
{
    if (!administrator() || row < 0 || row >= m_pictures.size() || m_pictures.at(row).stamp.isEmpty()) {
        return;
    }
    const PictureOf asked = m_pictures.at(row);
    const quint64 turn = ++m_pictureTurn;
    m_pictureImage = {};
    m_picture = {{u"open"_s, true},
                 {u"title"_s, asked.title},
                 {u"at"_s, asked.at.isEmpty() ? QString() : tr("Seen by the wall %1.").arg(asked.at)},
                 {u"state"_s, u"loading"_s},
                 {u"problem"_s, QString()}};
    emit pictureChanged();
    const QPointer<DetailsView> self(this);
    if (m_own != nullptr) {
        // (Kept beside this PC's settings for 30 days, the last alert's of each KVM: read in the watches' own thread.)
        m_own->askPicture(m_machine, asked.stamp, [self, turn](const std::optional<QByteArray>& bytes) {
            if (self != nullptr) {
                self->pictureBytes(turn, bytes ? base::Result<QByteArray>(*bytes)
                                               : base::Result<QByteArray>(base::Error{"no_picture", ""}));
            }
        });
        return;
    }
    m_client->askAlertPicture(m_machine, asked.stamp, [self, turn](const base::Result<QByteArray>& answer) {
        if (self != nullptr) {
            self->pictureBytes(turn, answer);
        }
    });
}

void DetailsView::pictureBytes(quint64 turn, const base::Result<QByteArray>& answer)
{
    if (turn != m_pictureTurn) {
        return;
    }
    if (!answer) {
        const bool gone = answer.error().code == "no_picture" || answer.error().code == "not_found";
        m_picture[u"state"_s] = gone ? u"gone"_s : u"failed"_s;
        m_picture[u"problem"_s] = QString::fromStdString(answer.error().message);
        emit pictureChanged();
        return;
    }
    // Decoded beside the interface, which goes on meanwhile; told back on it.
    const QPointer<DetailsView> self(this);
    QThreadPool::globalInstance()->start([self, turn, bytes = answer.value()] {
        const QImage image = pictureFrom(bytes);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, turn, image] {
                if (self != nullptr) {
                    self->pictureCame(turn, image);
                }
            },
            Qt::QueuedConnection);
    });
}

void DetailsView::pictureCame(quint64 turn, const QImage& image)
{
    if (turn != m_pictureTurn || !m_picture.value(u"open"_s).toBool()) {
        return;
    }
    m_pictureImage = image;
    m_picture[u"state"_s] = image.isNull() ? u"failed"_s : u"shown"_s;
    m_picture[u"problem"_s] = image.isNull() ? tr("The server sent a picture that could not be shown.") : QString();
    emit pictureChanged();
}

void DetailsView::closePicture()
{
    ++m_pictureTurn;
    if (m_picture.isEmpty() && m_pictureImage.isNull()) {
        return;
    }
    m_picture.clear();
    m_pictureImage = {};
    emit pictureChanged();
}
} // namespace qkvm::ui
