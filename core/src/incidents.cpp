#include <qkvm/core/incidents.h>

#include <algorithm>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace qkvm::core {
namespace {
bool token(const QString& value)
{
    return !value.isEmpty() && value.size() <= 256 &&
           std::ranges::none_of(value, [](QChar ch) { return ch.unicode() < 32; });
}
QString joined(const QString& scope, const QString& key)
{
    return scope + u'\n' + key;
}
QString nameOf(QString name)
{
    name = name.left(256);
    for (QChar& ch : name) {
        if (ch.unicode() < 32) {
            ch = u' ';
        }
    }
    return name;
}
QString stamp(const QDateTime& time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}
QDateTime timeOf(const QJsonValue& value)
{
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs).toUTC();
}
bool optionalTime(const QJsonValue& value)
{
    return value.isString() && (value.toString().isEmpty() || timeOf(value).isValid());
}
} // namespace

Incidents::Incidents(QString file, std::function<QDateTime()> clock, QObject* parent)
    : QObject(parent), m_file(std::move(file)), m_clock(std::move(clock))
{
    m_write.setSingleShot(true);
    connect(&m_write, &QTimer::timeout, this, [this] { flush(); });
    load();
}

Incidents::~Incidents()
{
    flush();
}

bool Incidents::knownState(const QString& state)
{
    return state == "online"_L1 || state == "offline"_L1 || state == "no_signal"_L1 || state == "auth"_L1 ||
           state == "error"_L1 || state == "health-warning"_L1 || state == "health-critical"_L1 || isScreenState(state);
}

bool Incidents::isScreenState(const QString& state)
{
    return state == "screen-no_signal"_L1 || state == "screen-unchanged"_L1;
}

bool Incidents::screenAlert(const QString& scope, const QString& key, const QString& name, const QString& watch,
                            bool back, int minutes)
{
    if (!token(scope) || !token(key) || (watch != "no_signal"_L1 && watch != "unchanged"_L1)) {
        return false;
    }
    const QString screen = screenKey(key, watch);
    const QString id = joined(scope, screen);
    if (!m_watches.contains(id)) {
        if (back || m_watches.size() >= mostTracked) {
            return false;
        }
        m_watches.insert(id, Watch{scope, screen, nameOf(name), u"online"_s, {}, {}, 0, false, {}});
    }
    Watch& one = m_watches[id];
    one.name = nameOf(name);
    one.pending.clear();
    one.count = 0;
    one.restored = false;
    one.stable = back ? u"online"_s : u"screen-"_s + watch;
    transition(one);
    if (!back) {
        for (Entry& entry : m_entries) {
            if (entry.scope == scope && entry.key == screen && !entry.closed.isValid()) {
                entry.minutes = std::clamp(minutes, 0, 1440);
            }
        }
    }
    trim();
    dirty();
    return pausedUntil(scope, key) <= now() && pausedUntil(scope, screen) <= now();
}

void Incidents::begin(const QString& scope)
{
    if (!token(scope)) {
        return;
    }
    if (!m_grace.contains(scope) && m_grace.size() >= mostTracked) {
        m_grace.removeIf([this](auto entry) { return entry.value() <= now(); });
        if (m_grace.size() >= mostTracked)
            return;
    }
    m_grace[scope] = now().addSecs(120);
    for (Watch& watch : m_watches) {
        if (watch.scope == scope) {
            watch.pending.clear();
            watch.count = 0;
            watch.restored = true;
        }
    }
}

void Incidents::dirty()
{
    m_dirty = true;
    if (!m_write.isActive()) {
        m_write.start(250);
    }
    emit changed();
}

QList<Incidents::Change> Incidents::observe(const QString& scope, const QList<Observation>& observations)
{
    QList<Change> changes;
    if (!token(scope)) {
        return changes;
    }
    QSet<QString> seen;
    bool altered = false;
    for (const Observation& observation : observations) {
        if (!token(observation.key) || seen.contains(observation.key)) {
            continue;
        }
        seen.insert(observation.key);
        const QString id = joined(scope, observation.key);
        if (!m_watches.contains(id)) {
            if (m_watches.size() >= mostTracked) {
                m_problem = tr("The incident monitoring limit has been reached.");
                emit changed();
                continue;
            }
            m_watches.insert(id, Watch{scope, observation.key, nameOf(observation.name), {}, {}, {}, 0, false, {}});
        }
        Watch& watch = m_watches[id];
        const QString name = nameOf(observation.name);
        if (watch.name != name) {
            watch.name = name;
            altered = true;
        }
        if (!knownState(observation.state)) {
            watch.pending.clear();
            watch.count = 0;
            continue;
        }
        const bool muted = pausedUntil(scope, watch.key) > now();
        if (watch.stable == observation.state) {
            watch.pending.clear();
            watch.count = 0;
            watch.restored = false;
        } else {
            if (watch.restored && now() < m_grace.value(scope)) {
                continue;
            }
            watch.count = watch.pending == observation.state ? watch.count + 1 : 1;
            watch.pending = observation.state;
            if (watch.count < 2) {
                continue;
            }
            const QString previous = watch.stable;
            watch.stable = observation.state;
            watch.pending.clear();
            watch.count = 0;
            watch.restored = false;
            transition(watch);
            altered = true;
            if (!previous.isEmpty()) {
                if (muted) {
                    if (watch.held.isEmpty()) {
                        watch.held = previous;
                    }
                } else if (watch.held.isEmpty()) {
                    changes.append({watch.key, watch.name, watch.stable});
                }
            }
        }
        // Maintenance retains the net change, including across a restart. Resume only after a
        // fresh settled observation, never because a timer re-read a cached failed picture.
        if (!muted && !watch.held.isEmpty()) {
            if (watch.held != watch.stable) {
                changes.append({watch.key, watch.name, watch.stable});
            }
            watch.held.clear();
            altered = true;
        }
    }
    for (Watch& watch : m_watches) {
        if (watch.scope == scope && !seen.contains(watch.key)) {
            watch.pending.clear();
            watch.count = 0;
        }
    }
    if (altered) {
        trim();
        dirty();
    }
    return changes;
}

void Incidents::transition(const Watch& watch)
{
    auto open = std::ranges::find_if(m_entries, [&](const Entry& entry) {
        return entry.scope == watch.scope && entry.key == watch.key && !entry.closed.isValid();
    });
    if (watch.stable == "online"_L1) {
        if (open != m_entries.end()) {
            open->closed = now();
            open->changed = now();
        }
        return;
    }
    if (open == m_entries.end()) {
        m_entries.append({QUuid::createUuid().toString(QUuid::WithoutBraces),
                          watch.scope,
                          watch.key,
                          watch.name,
                          watch.stable,
                          now(),
                          now(),
                          {},
                          false,
                          false});
    } else {
        open->name = watch.name;
        open->state = watch.stable;
        open->changed = now();
        open->acknowledged = false;
    }
}

void Incidents::reconcile(const QString& scope, const QSet<QString>& keys)
{
    bool altered = false;
    for (auto it = m_watches.begin(); it != m_watches.end();) {
        if (it->scope == scope && !keys.contains(it->key)) {
            it = m_watches.erase(it);
            altered = true;
        } else {
            ++it;
        }
    }
    for (Entry& entry : m_entries) {
        if (entry.scope == scope && !keys.contains(entry.key) && !entry.closed.isValid()) {
            entry.closed = now();
            entry.changed = now();
            entry.removed = true;
            altered = true;
        }
    }
    if (altered) {
        trim();
        dirty();
    }
}

QList<Incidents::Entry> Incidents::entries(const QString& scope) const
{
    QList<Entry> result;
    for (const Entry& entry : m_entries) {
        if (entry.scope == scope) {
            result.append(entry);
        }
    }
    std::ranges::stable_sort(result, [](const Entry& a, const Entry& b) {
        if (a.closed.isValid() != b.closed.isValid()) {
            return !a.closed.isValid();
        }
        return a.changed > b.changed;
    });
    return result;
}

bool Incidents::acknowledge(const QString& scope, const QString& id)
{
    for (Entry& entry : m_entries) {
        if (entry.scope == scope && entry.id == id && !entry.acknowledged) {
            entry.acknowledged = true;
            dirty();
            return true;
        }
    }
    return false;
}

void Incidents::pause(const QString& scope, const QString& key, bool paused)
{
    if (!token(scope)) {
        return;
    }
    const QDateTime until = paused ? now().addSecs(3600) : QDateTime();
    if (key.isEmpty()) {
        if (!m_pauses.contains(scope) && m_pauses.size() >= mostTracked) {
            return;
        }
        m_pauses[scope] = until;
    } else {
        auto watch = m_watches.find(joined(scope, key));
        if (watch == m_watches.end()) {
            return;
        }
        watch->muted = until;
    }
    dirty();
}

QDateTime Incidents::pausedUntil(const QString& scope, const QString& key) const
{
    const auto watch = m_watches.constFind(joined(scope, key));
    const QDateTime one = watch == m_watches.constEnd() ? QDateTime() : watch->muted;
    const QDateTime all = m_pauses.value(scope);
    return one > all ? one : all;
}

void Incidents::trim()
{
    std::ranges::stable_sort(m_entries, [](const Entry& a, const Entry& b) { return a.changed > b.changed; });
    int closed = 0;
    m_entries.removeIf([&](const Entry& entry) { return entry.closed.isValid() && ++closed > mostClosed; });
}

void Incidents::load()
{
    QFile file(m_file);
    if (!file.exists()) {
        return;
    }
    const auto refuse = [this] {
        m_damaged = true;
        m_problem = tr("Saved incident history could not be read. It has been kept unchanged.");
        m_watches.clear();
        m_entries.clear();
        m_pauses.clear();
    };
    if (!file.open(QIODevice::ReadOnly) || file.size() > mostBytes) {
        refuse();
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(mostBytes + 1));
    const QJsonObject root = doc.object();
    const QJsonArray watches = root[u"watches"_s].toArray();
    const QJsonArray entries = root[u"entries"_s].toArray();
    const QJsonObject pauses = root[u"pauses"_s].toObject();
    if (root[u"version"_s] != 1 || !root[u"watches"_s].isArray() || !root[u"entries"_s].isArray() ||
        !root[u"pauses"_s].isObject() || watches.size() > mostTracked || entries.size() > mostTracked + mostClosed ||
        pauses.size() > mostTracked) {
        refuse();
        return;
    }
    for (const auto& value : watches) {
        const QJsonObject obj = value.toObject();
        Watch watch{obj[u"scope"_s].toString(),
                    obj[u"key"_s].toString(),
                    obj[u"name"_s].toString(),
                    obj[u"state"_s].toString(),
                    {},
                    obj[u"held"_s].toString(),
                    0,
                    true,
                    timeOf(obj[u"muted"_s])};
        const QString key = joined(watch.scope, watch.key);
        if (!token(watch.scope) || !token(watch.key) || !obj[u"name"_s].isString() || watch.name.size() > 256 ||
            !obj[u"held"_s].isString() || !optionalTime(obj[u"muted"_s]) || !knownState(watch.stable) ||
            (!watch.held.isEmpty() && !knownState(watch.held)) || m_watches.contains(key)) {
            refuse();
            return;
        }
        watch.name = nameOf(watch.name);
        m_watches.insert(key, watch);
    }
    QSet<QString> ids;
    QSet<QString> open;
    for (const auto& value : entries) {
        const QJsonObject obj = value.toObject();
        Entry entry{obj[u"id"_s].toString(),   obj[u"scope"_s].toString(), obj[u"key"_s].toString(),
                    obj[u"name"_s].toString(), obj[u"state"_s].toString(), timeOf(obj[u"opened"_s]),
                    timeOf(obj[u"changed"_s]), timeOf(obj[u"closed"_s]),   obj[u"ack"_s].toBool(),
                    obj[u"removed"_s].toBool()};
        // (A screen watch's minutes, where one is said: a whole number of 0 to 1440.)
        if (const QJsonValue minutes = obj[u"minutes"_s]; !minutes.isUndefined()) {
            if (!minutes.isDouble() || minutes.toInteger(-1) < 0 || minutes.toInteger(-1) > 1440) {
                refuse();
                return;
            }
            entry.minutes = static_cast<int>(minutes.toInteger());
        }
        const QString key = joined(entry.scope, entry.key);
        if (!token(entry.id) || ids.contains(entry.id) || !token(entry.scope) || !token(entry.key) ||
            !obj[u"name"_s].isString() || entry.name.size() > 256 || !knownState(entry.state) ||
            entry.state == "online"_L1 || !entry.opened.isValid() || !entry.changed.isValid() ||
            !obj[u"ack"_s].isBool() || !obj[u"removed"_s].isBool() || !optionalTime(obj[u"closed"_s]) ||
            (!entry.closed.isValid() && (entry.removed || open.contains(key) || !m_watches.contains(key) ||
                                         m_watches.value(key).stable != entry.state))) {
            refuse();
            return;
        }
        ids.insert(entry.id);
        if (!entry.closed.isValid()) {
            open.insert(key);
        }
        entry.name = nameOf(entry.name);
        m_entries.append(entry);
    }
    for (auto it = pauses.begin(); it != pauses.end(); ++it) {
        if (!token(it.key()) || !optionalTime(it.value())) {
            refuse();
            return;
        }
        m_pauses.insert(it.key(), timeOf(it.value()));
    }
    for (auto it = m_watches.cbegin(); it != m_watches.cend(); ++it) {
        if (it->stable != "online"_L1 && !open.contains(it.key())) {
            refuse();
            return;
        }
    }
    trim();
}

bool Incidents::flush()
{
    m_write.stop();
    if (!m_dirty) {
        return !m_damaged;
    }
    if (m_damaged) {
        return false;
    }
    QJsonArray watches;
    for (const Watch& watch : std::as_const(m_watches)) {
        if (!watch.stable.isEmpty()) {
            watches.append(QJsonObject{{u"scope"_s, watch.scope},
                                       {u"key"_s, watch.key},
                                       {u"name"_s, watch.name},
                                       {u"state"_s, watch.stable},
                                       {u"held"_s, watch.held},
                                       {u"muted"_s, stamp(watch.muted)}});
        }
    }
    QJsonArray entries;
    for (const Entry& entry : std::as_const(m_entries)) {
        QJsonObject written{{u"id"_s, entry.id},
                            {u"scope"_s, entry.scope},
                            {u"key"_s, entry.key},
                            {u"name"_s, entry.name},
                            {u"state"_s, entry.state},
                            {u"opened"_s, stamp(entry.opened)},
                            {u"changed"_s, stamp(entry.changed)},
                            {u"closed"_s, stamp(entry.closed)},
                            {u"ack"_s, entry.acknowledged},
                            {u"removed"_s, entry.removed}};
        if (entry.minutes > 0) {
            written.insert(u"minutes"_s, entry.minutes);
        }
        entries.append(written);
    }
    QJsonObject pauses;
    for (auto it = m_pauses.cbegin(); it != m_pauses.cend(); ++it) {
        pauses.insert(it.key(), stamp(it.value()));
    }
    const QByteArray bytes =
        QJsonDocument(
            QJsonObject{{u"version"_s, 1}, {u"watches"_s, watches}, {u"entries"_s, entries}, {u"pauses"_s, pauses}})
            .toJson(QJsonDocument::Compact);
    QSaveFile file(m_file);
    file.setDirectWriteFallback(false);
    if (bytes.size() > mostBytes || !QDir().mkpath(QFileInfo(m_file).absolutePath()) ||
        !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_problem = tr("Incident history could not be saved. Monitoring continues; saving will be retried.");
        m_write.start(60000);
        emit changed();
        return false;
    }
    m_dirty = false;
    m_problem.clear();
    emit changed();
    return true;
}

bool Incidents::repair()
{
    if (!m_damaged) {
        return true;
    }
    const QString backup = m_file + u".damaged-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QFile::rename(m_file, backup)) {
        m_problem = tr("The unreadable history could not be kept as a backup. No file was replaced.");
        emit changed();
        return false;
    }
    m_damaged = false;
    m_dirty = true;
    return flush();
}
} // namespace qkvm::core
