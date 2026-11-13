#include "discovery_view.h"

#include <QVariantMap>

#include <qkvm/core/kvm_adding.h>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

DiscoveryView::DiscoveryView(QObject* parent)
    : QAbstractListModel(parent), m_scanner(new core::Discovery(this)),
      m_networksProvider([] { return core::Discovery::networks(); })
{
    listen();
}

DiscoveryView::DiscoveryView(core::Discovery& scanner, const QList<core::Discovery::Network>& networks, QObject* parent)
    : QAbstractListModel(parent), m_scanner(&scanner), m_networksProvider([networks] { return networks; })
{
    listen();
}

DiscoveryView::~DiscoveryView()
{
    if (m_scanner != nullptr) {
        disconnect(m_scanner, nullptr, this, nullptr);
        m_scanner->cancel();
    }
}

void DiscoveryView::listen()
{
    connect(m_scanner, &core::Discovery::progress, this, [this](int completed, int total) {
        m_completed = completed;
        m_total = total;
        emit changed();
    });
    connect(m_scanner, &core::Discovery::found, this, [this](const core::Discovery::Device& device) {
        if (!m_active || !m_busy) {
            return;
        }
        const int row = count();
        beginInsertRows({}, row, row);
        m_devices.append(device);
        endInsertRows();
        emit changed();
    });
    connect(m_scanner, &core::Discovery::problem, this, [this](const QString& problem) {
        m_problem = problem;
        emit changed();
    });
    connect(m_scanner, &core::Discovery::finished, this, [this](bool cancelled) {
        m_busy = false;
        m_state = cancelled ? u"stopped"_s : m_problem.isEmpty() ? u"complete"_s : u"failed"_s;
        emit changed();
    });
    connect(m_scanner, &QObject::destroyed, this, [this] {
        m_busy = false;
        m_state = u"stopped"_s;
        emit changed();
    });
}

QVariantList DiscoveryView::networks() const
{
    QVariantList rows;
    for (int index = 0; index < m_networks.size(); ++index) {
        const core::Discovery::Network& network = m_networks.at(index);
        rows.append(QVariantMap{{u"label"_s, network.label},
                                {u"address"_s, network.address},
                                {u"cidr"_s, network.cidr},
                                {u"hostCount"_s, QVariant::fromValue(network.hostCount)},
                                {u"selected"_s, m_selected.contains(index)}});
    }
    return rows;
}

void DiscoveryView::setManualScope(const QString& scope)
{
    if (m_busy || m_manualScope == scope) {
        return;
    }
    m_manualScope = scope;
    m_problem.clear();
    forgetConfirmation();
    emit changed();
}

void DiscoveryView::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (!active) {
        stop();
        forgetConfirmation();
    }
    emit changed();
}

void DiscoveryView::forgetResults()
{
    beginResetModel();
    m_devices.clear();
    endResetModel();
}

void DiscoveryView::forgetConfirmation()
{
    m_confirmationNeeded = false;
    m_planned = 0;
}

void DiscoveryView::prepare()
{
    closed();
    m_networks = m_networksProvider();
    emit networksChanged();
}

void DiscoveryView::closed()
{
    stop();
    forgetResults();
    m_networks.clear();
    m_selected.clear();
    m_manualScope.clear();
    m_problem.clear();
    m_state = u"idle"_s;
    m_completed = 0;
    m_total = 0;
    forgetConfirmation();
    emit networksChanged();
    emit changed();
}

void DiscoveryView::selectNetwork(int index, bool selected)
{
    if (m_busy || index < 0 || index >= m_networks.size()) {
        return;
    }
    if (selected) {
        m_selected.insert(index);
    } else {
        m_selected.remove(index);
    }
    m_problem.clear();
    forgetConfirmation();
    emit networksChanged();
    emit changed();
}

QStringList DiscoveryView::scopes() const
{
    QStringList chosen;
    for (int index = 0; index < m_networks.size(); ++index) {
        if (m_selected.contains(index)) {
            chosen.append(m_networks.at(index).cidr);
        }
    }
    if (!m_manualScope.trimmed().isEmpty()) {
        chosen.append(m_manualScope.trimmed());
    }
    return chosen;
}

void DiscoveryView::start(bool confirmed)
{
    if (!m_active || m_busy || m_scanner == nullptr) {
        return;
    }
    const QStringList chosen = scopes();
    const auto plan = core::Discovery::planFor(chosen);
    if (!plan) {
        m_problem = QString::fromStdString(plan.error().message);
        forgetConfirmation();
        emit changed();
        return;
    }
    m_problem.clear();
    if (plan.value().needsConfirmation && (!confirmed || !m_confirmationNeeded)) {
        m_confirmationNeeded = true;
        m_planned = plan.value().total;
        emit changed();
        return;
    }
    const auto started = m_scanner->start(chosen, confirmed && m_confirmationNeeded);
    if (!started) {
        m_problem = QString::fromStdString(started.error().message);
        forgetConfirmation();
        emit changed();
        return;
    }
    forgetResults();
    forgetConfirmation();
    m_completed = 0;
    m_total = plan.value().total;
    m_busy = true;
    m_state = u"searching"_s;
    emit changed();
}

void DiscoveryView::stop()
{
    if (m_scanner != nullptr && m_busy) {
        m_scanner->cancel();
    }
}

void DiscoveryView::choose(int index)
{
    // (Only of a family this program adds by itself: a kvmd, or a JetKVM.)
    if (!m_active || index < 0 || index >= m_devices.size() ||
        core::kvm::familyOf(m_devices.at(index).kind).isEmpty()) {
        return;
    }
    const QString address = m_devices.at(index).address.toString(QUrl::FullyEncoded);
    const QString family = core::kvm::familyOf(m_devices.at(index).kind);
    stop();
    emit addressChosen(address, family);
}

int DiscoveryView::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant DiscoveryView::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_devices.size()) {
        return {};
    }
    const core::Discovery::Device& device = m_devices.at(index.row());
    switch (role) {
    case AddressRole:
        return device.address.toString(QUrl::FullyEncoded);
    case FamilyRole:
        switch (device.kind) {
        case core::KvmProbe::Kind::Kvmd:
            return u"kvmd"_s;
        case core::KvmProbe::Kind::JetKvm:
            return u"jetkvm"_s;
        case core::KvmProbe::Kind::Redfish:
            return u"redfish"_s;
        default:
            return QString();
        }
    case SupportedRole:
        return !core::kvm::familyOf(device.kind).isEmpty();
    case EncryptedRole:
        return device.address.scheme() == u"https";
    default:
        return {};
    }
}

QHash<int, QByteArray> DiscoveryView::roleNames() const
{
    return {
        {AddressRole, "address"}, {FamilyRole, "family"}, {SupportedRole, "supported"}, {EncryptedRole, "encrypted"}};
}

} // namespace qkvm::ui
