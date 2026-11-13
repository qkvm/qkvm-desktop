#include "alerts_view.h"

#include <algorithm>

#include <QDateTime>
#include <QLocale>
#include <QMap>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace qkvm::ui {
namespace {
QString when(const QDateTime& at)
{
    return at.isValid() ? QLocale().toString(at.toLocalTime(), QLocale::ShortFormat) : QString();
}
} // namespace

AlertsView::AlertsView(core::Incidents& incidents, AppLock& lock, QObject* parent)
    : QObject(parent), m_incidents(incidents), m_lock(lock)
{
    connect(&incidents, &core::Incidents::changed, this, &AlertsView::changed);
    connect(&lock, &AppLock::changed, this, &AlertsView::changed);
    m_expiry.setInterval(1000);
    connect(&m_expiry, &QTimer::timeout, this, [this] {
        QSet<QString> muted;
        if (paused()) {
            muted.insert({});
        }
        for (const auto& entry : m_incidents.entries(m_scope)) {
            if (m_incidents.pausedUntil(m_scope, entry.key) > m_incidents.now()) {
                muted.insert(entry.key);
            }
        }
        if (muted != m_lastMuted) {
            m_lastMuted = muted;
            emit changed();
        }
    });
    m_expiry.start();
}

void AlertsView::watch(const QString& scope, bool local, const QString& name)
{
    if (scope.isEmpty())
        return;
    if (!m_channels.contains(scope))
        m_incidents.begin(scope);
    auto& channel = m_channels[scope];
    channel.local = local;
    channel.name = name;
    channel.active = true;
    channel.problem.clear();
    emit changed();
}

void AlertsView::unavailable(const QString& scope, const QString& name, const QString& reason)
{
    auto& channel = m_channels[scope];
    channel.name = name;
    channel.problem = reason;
    channel.active = false;
    channel.machines.clear();
    emit changed();
}

void AlertsView::forgetSource(const QString& scope)
{
    m_channels.remove(scope);
    emit changed();
}

QString AlertsView::sourceStatus() const
{
    if (m_lock.restricted() || !m_channels.contains(m_scope))
        return {};
    const auto channel = m_channels.value(m_scope);
    return !channel.problem.isEmpty() ? channel.problem
           : channel.active
               ? (channel.inContact ? tr("Monitoring in the background") : tr("Connection lost; checking again"))
               : tr("Monitoring paused; open this source to reconnect");
}

void AlertsView::openSource()
{
    if (!m_lock.restricted() && m_channels.contains(m_scope))
        emit openAsked(m_scope);
}

void AlertsView::suspend(const QString& scope)
{
    if (auto found = m_channels.find(scope); found != m_channels.end()) {
        found->active = false;
        found->machines.clear();
        emit changed();
    }
}

QVariantList AlertsView::sources() const
{
    QVariantList result;
    if (m_lock.restricted())
        return result;
    QStringList keys = m_channels.keys();
    keys.sort();
    for (const auto& key : keys) {
        const auto& channel = m_channels[key];
        if (channel.active || !channel.problem.isEmpty())
            result.append(QVariantMap{{u"value"_s, key}, {u"label"_s, channel.name}});
    }
    return result;
}

QVariantMap AlertsView::treeSource(const QString& scope) const
{
    if (m_lock.restricted() || !m_channels.contains(scope))
        return {};
    const auto& channel = m_channels[scope];
    QVariantList rows;
    if (channel.active) {
        QMap<QString, QList<core::Machine>> groups;
        for (const auto& machine : channel.machines)
            groups[machine.group].append(machine);
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            auto& machines = it.value();
            std::ranges::sort(machines, [](const auto& a, const auto& b) {
                const auto compared = QString::compare(a.name, b.name, Qt::CaseInsensitive);
                return compared == 0 ? a.id < b.id : compared < 0;
            });
            const auto group = scope + u"/g/"_s + it.key();
            rows.append(QVariantMap{{u"key"_s, group},
                                    {u"depth"_s, 1},
                                    {u"parentKey"_s, scope},
                                    {u"label"_s, it.key().isEmpty() ? tr("Ungrouped") : it.key()},
                                    {u"status"_s, tr("%n machine(s)", nullptr, static_cast<int>(machines.size()))}});
            for (const auto& machine : machines) {
                const auto state = channel.states.value(machine.id, u"connecting"_s);
                rows.append(QVariantMap{{u"key"_s, scope + u"/m/"_s + machine.id},
                                        {u"depth"_s, 2},
                                        {u"parentKey"_s, group},
                                        {u"ancestorKey"_s, scope},
                                        {u"scope"_s, scope},
                                        {u"machine"_s, machine.id},
                                        {u"label"_s, machine.name},
                                        {u"status"_s, !channel.inContact     ? tr("Status unavailable")
                                                      : state == u"online"_s ? tr("Online")
                                                                             : label(state)}});
            }
        }
    }
    const auto status = !channel.problem.isEmpty()   ? channel.problem
                        : !channel.active            ? tr("Open to reconnect")
                        : !channel.inContact         ? tr("Connection lost; checking again")
                        : channel.machines.isEmpty() ? tr("No machines")
                                                     : tr("Connected");
    return {{u"status"_s, status}, {u"rows"_s, rows}};
}

QList<MachineChoice> AlertsView::availableMachines() const
{
    QList<MachineChoice> result;
    if (m_lock.restricted())
        return result;
    QStringList scopes = m_channels.keys();
    scopes.sort();
    for (const auto& scope : scopes) {
        const auto& channel = m_channels[scope];
        if (!channel.active)
            continue;
        QStringList machines = channel.machines.keys();
        machines.sort();
        for (const auto& id : machines) {
            if (!id.isEmpty() && id != "@source"_L1 && id.size() <= 512)
                result.append({{scope, id}, channel.machines.value(id).name, channel.name});
        }
    }
    return result;
}

void AlertsView::choose(const QString& scope)
{
    if (!m_lock.restricted() && m_channels.contains(scope)) {
        m_scope = scope;
        m_history = false;
        emit changed();
    }
}

void AlertsView::context(const QString& scope, bool local)
{
    if (m_scope == scope)
        return;
    if (!scope.isEmpty() && !m_channels.contains(scope))
        watch(scope, local, local ? tr("This PC") : tr("qKVM server"));
    m_scope = scope;
    m_history = false;
    emit changed();
}

void AlertsView::inventory(const QList<core::Machine>& machines)
{
    inventoryFor(m_scope, machines);
}

void AlertsView::inventoryFor(const QString& scope, const QList<core::Machine>& machines)
{
    if (!m_channels.contains(scope))
        return;
    auto& channel = m_channels[scope];
    channel.machines.clear();
    QSet<QString> keys;
    for (const auto& machine : machines) {
        channel.machines.insert(machine.id, machine);
        keys.insert(machine.id);
        // (And its screen watches' entries, where it is watched: one switched off is no longer monitored.)
        if (machine.noSignalMinutes > 0) {
            keys.insert(core::Incidents::screenKey(machine.id, u"no_signal"_s));
        }
        if (machine.unchangedMinutes > 0) {
            keys.insert(core::Incidents::screenKey(machine.id, u"unchanged"_s));
        }
    }
    if (!channel.local)
        keys.insert(u"@source"_s);
    m_incidents.reconcile(scope, keys);
    emit changed();
}

void AlertsView::observe(const QHash<QString, core::Status>& status, bool inContact)
{
    observeFor(m_scope, status, inContact);
}

void AlertsView::observeFor(const QString& scope, const QHash<QString, core::Status>& status, bool inContact)
{
    if (!m_channels.contains(scope) || !m_channels.value(scope).active)
        return;
    auto& channel = m_channels[scope];
    if (inContact && !channel.inContact)
        m_incidents.begin(scope);
    const bool contactChanged = channel.inContact != inContact;
    channel.inContact = inContact;
    QHash<QString, QString> states;
    for (auto it = status.cbegin(); it != status.cend(); ++it)
        states.insert(it.key(), it->state);
    const bool statesChanged = channel.states != states;
    channel.states = states;
    if (contactChanged || statesChanged)
        emit changed();
    QList<core::Incidents::Observation> observations;
    if (!channel.local)
        observations.append({u"@source"_s, channel.name, inContact ? u"online"_s : u"offline"_s});
    if (inContact) {
        for (auto it = channel.machines.cbegin(); it != channel.machines.cend(); ++it) {
            const auto found = status.constFind(it.key());
            if (found == status.constEnd())
                continue;
            QString state = found->state;
            // Where its screen is watched for no signal, that is the watch's to tell, after its minutes, and not the
            // alerts' after two looks (the web's screens.py).
            if (state == "no_signal"_L1 && it->noSignalMinutes > 0) {
                state = u"online"_s;
            }
            const auto& server = found->server;
            if (state == "online"_L1 && server) {
                if (server->health == "Critical"_L1)
                    state = u"health-critical"_s;
                else if (server->health == "Warning"_L1)
                    state = u"health-warning"_s;
            }
            observations.append({it.key(), it->name, state});
        }
    }
    const QString sourceName = channel.name;
    const auto changes = m_incidents.observe(scope, observations);
    if (!changes.isEmpty() && !m_lock.restricted()) {
        QStringList names;
        bool warning = false;
        for (const auto& change : changes) {
            warning = warning || change.state != "online"_L1;
            if (names.size() < 3)
                names.append(change.name.left(60) + u": "_s + label(change.state));
        }
        emit notification(
            changes.size() == 1 ? tr("%1: %2").arg(changes.front().name.left(40), label(changes.front().state))
                                : tr("%1 alert changes").arg(changes.size()),
            sourceName.left(80) + u" - "_s + (changes.size() == 1 ? tr("Click to open in qKVM.") : names.join(u"; "_s)),
            warning, scope, changes.size() == 1 ? changes.front().key : QString());
    }
}

void AlertsView::screenAlertFor(const QString& scope, const core::ScreenWatch::Event& event)
{
    if (!m_channels.contains(scope) || !m_channels.value(scope).active) {
        return;
    }
    const Channel& channel = m_channels[scope];
    const auto machine = channel.machines.constFind(event.machine);
    if (machine == channel.machines.constEnd()) {
        return;
    }
    const bool tell =
        m_incidents.screenAlert(scope, event.machine, machine->name, event.watch, event.back, event.minutes);
    if (tell && !m_lock.restricted()) {
        emit notification(tr("%1: %2").arg(machine->name.left(40), watchText(event.watch, event.back, event.minutes)),
                          channel.name.left(80) + u" - "_s + tr("Click to open in qKVM."), !event.back, scope,
                          event.machine);
    }
}

QString AlertsView::watchText(const QString& watch, bool back, int minutes)
{
    const QString lasting = minutes == 1 ? tr("1 minute") : tr("%1 minutes").arg(minutes);
    if (watch == "no_signal"_L1) {
        return back          ? tr("Back to normal: a picture again")
               : minutes > 0 ? tr("No signal for %1").arg(lasting)
                             : tr("No signal");
    }
    return back          ? tr("Back to normal: the screen changes again")
           : minutes > 0 ? tr("Screen unchanged for %1").arg(lasting)
                         : tr("Screen unchanged");
}

QString AlertsView::label(const QString& state)
{
    if (state == "online"_L1)
        return tr("Recovered");
    if (state == "offline"_L1)
        return tr("Offline");
    if (state == "no_signal"_L1)
        return tr("No video signal / powered off");
    if (state == "auth"_L1)
        return tr("Login rejected");
    if (state == "health-warning"_L1)
        return tr("Server health warning");
    if (state == "health-critical"_L1)
        return tr("Server health critical");
    return tr("Device problem");
}

bool AlertsView::visible(const core::Incidents::Entry& entry) const
{
    // A server's reduced inventory must not reveal names from an earlier permission set.
    const auto& channel = m_channels[m_scope];
    return channel.local || entry.key == "@source"_L1 || channel.machines.contains(entry.key);
}

QVariantList AlertsView::entries() const
{
    QVariantList result;
    if (!allowed())
        return result;
    for (const auto& entry : m_incidents.entries(m_scope)) {
        if (!visible(entry) || (!m_history && entry.closed.isValid()))
            continue;
        const bool muted = m_incidents.pausedUntil(m_scope, entry.key) > m_incidents.now();
        // (A screen watch's under the KVM's own key and a mark: paused with the KVM.)
        const bool screen = core::Incidents::isScreenState(entry.state);
        const QString machine = screen ? entry.key.section(u'#', 0, 0) : entry.key;
        result.append(QVariantMap{
            {u"id"_s, entry.id},
            {u"name"_s, entry.name},
            {u"state"_s, screen ? watchText(entry.state.mid(7), false, entry.minutes) : label(entry.state)},
            {u"opened"_s, when(entry.opened)},
            {u"changed"_s, when(entry.changed)},
            {u"closed"_s, when(entry.closed)},
            {u"active"_s, !entry.closed.isValid()},
            {u"acknowledged"_s, entry.acknowledged},
            {u"muted"_s, muted},
            {u"removed"_s, entry.removed},
            {u"canPause"_s, m_channels.value(m_scope).machines.contains(machine) || entry.key == "@source"_L1}});
    }
    return result;
}

int AlertsView::attention() const
{
    if (!allowed())
        return 0;
    int count = 0;
    for (const auto& entry : m_incidents.entries(m_scope)) {
        if (visible(entry) && !entry.closed.isValid() && !entry.acknowledged &&
            m_incidents.pausedUntil(m_scope, entry.key) <= m_incidents.now())
            ++count;
    }
    return count;
}

bool AlertsView::paused() const
{
    return allowed() && m_incidents.pausedUntil(m_scope) > m_incidents.now();
}
QString AlertsView::pauseEnds() const
{
    return paused() ? when(m_incidents.pausedUntil(m_scope)) : QString();
}
void AlertsView::setHistory(bool history)
{
    if (m_history != history) {
        m_history = history;
        emit changed();
    }
}
void AlertsView::acknowledge(const QString& id)
{
    if (!allowed())
        return;
    for (const auto& entry : m_incidents.entries(m_scope)) {
        if (entry.id == id && visible(entry)) {
            m_incidents.acknowledge(m_scope, id);
            return;
        }
    }
}
void AlertsView::pause(bool paused)
{
    if (allowed())
        m_incidents.pause(m_scope, {}, paused);
}
void AlertsView::pauseDevice(const QString& id, bool paused)
{
    if (!allowed())
        return;
    for (const auto& entry : m_incidents.entries(m_scope)) {
        if (entry.id == id && visible(entry)) {
            m_incidents.pause(m_scope, entry.key, paused);
            return;
        }
    }
}
void AlertsView::repair()
{
    if (allowed())
        m_incidents.repair();
}
} // namespace qkvm::ui
