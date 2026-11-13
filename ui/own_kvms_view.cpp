#include "own_kvms_view.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QSet>
#include <QStringList>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

OwnKvmsView::OwnKvmsView(QObject* parent) : QAbstractListModel(parent) {}

void OwnKvmsView::use(core::LocalWall* wall)
{
    disconnect(m_heard);
    disconnect(m_cameToSomething);
    m_wall = wall;
    if (wall != nullptr) {
        // What the wall's KVMs are, while the list is shown; and what their drivers found, which
        // the wall says by itself when a first look has ended.
        m_heard = connect(wall, &core::LocalWall::changed, this, [this] {
            if (isShown()) {
                follow();
            }
        });
        m_cameToSomething = connect(wall, &core::LocalWall::cameToSomething, this, [this] {
            if (isShown()) {
                follow();
            }
        });
    }
    if (isShown()) {
        takeAnew();
    }
}

void OwnKvmsView::begin()
{
    if (++m_shownBy > 1) {
        return;
    }
    takeAnew();
}

void OwnKvmsView::end()
{
    if (m_shownBy > 0) {
        --m_shownBy;
    }
}

int OwnKvmsView::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant OwnKvmsView::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
    case KeyRole:
        return row.id;
    case NameRole:
        return row.name;
    case HostRole:
        return row.host;
    case WayRole:
        return row.way;
    case TwinsRole:
        return row.twins;
    case NoPasswordRole:
        return row.noPassword;
    case GoneRole:
        return row.gone;
    case FamilyRole:
        return row.family;
    default:
        return {};
    }
}

QHash<int, QByteArray> OwnKvmsView::roleNames() const
{
    return {{KeyRole, "key"},   {NameRole, "name"},    {HostRole, "host"},
            {WayRole, "way"},   {TwinsRole, "twins"},  {NoPasswordRole, "noPassword"},
            {GoneRole, "gone"}, {FamilyRole, "family"}};
}

OwnKvmsView::Alike OwnKvmsView::alikeIn(const QList<core::OwnKvm>& all)
{
    Alike alike;
    for (const core::OwnKvm& kvm : all) {
        if (kvm.knownByCertificate) {
            alike[kvm.certificate].append(kvm);
        }
    }
    return alike;
}

OwnKvmsView::Row OwnKvmsView::rowOf(const core::OwnKvm& kvm, const Alike& alike,
                                    const QSet<QString>& foundWithout) const
{
    Row row;
    row.id = kvm.id;
    row.host = core::LocalWall::hostOf(kvm);
    row.family = kvm.kind;
    // (One without a name is called by where it is, as its card calls it.)
    row.name = kvm.name.isEmpty() ? row.host : kvm.name;
    if (QUrl(kvm.address).scheme() != u"https"_s) {
        row.way = u"plain"_s;
    } else {
        row.way = kvm.knownByCertificate ? u"certificate"_s : u"windows"_s;
    }
    // Every other KVM of the wall that is the same device as this one, as the wall finds a twin,
    // among those that are known by its certificate. (Only one that is known by a certificate has
    // any, and only another that is known by the same one can be its twin.)
    if (kvm.knownByCertificate) {
        QList<core::OwnKvm> others = alike.value(kvm.certificate);
        others.removeIf([&kvm](const core::OwnKvm& one) { return one.id == kvm.id; });
        QStringList twins;
        for (std::optional<core::OwnKvm> twin = core::LocalWall::twinOf(others, kvm); twin.has_value();
             twin = core::LocalWall::twinOf(others, kvm)) {
            twins.append(core::LocalWall::calledInASentence(*twin));
            others.removeIf([&twin](const core::OwnKvm& one) { return one.id == twin->id; });
        }
        row.twins = twins.join(u", "_s);
    }
    // No password is kept for it where the wall's mark says none, and where its driver found none
    // at its last look, whatever the mark says. (Nothing of the store is read for either.)
    row.noPassword = m_wall != nullptr && (!m_wall->marksAPasswordFor(kvm.id) || foundWithout.contains(kvm.id));
    return row;
}

void OwnKvmsView::takeAnew()
{
    QList<Row> rows;
    if (m_wall != nullptr) {
        const QList<core::OwnKvm>& all = m_wall->kvms();
        const Alike alike = alikeIn(all);
        const QSet<QString> foundWithout = m_wall->foundWithoutAPassword();
        rows.reserve(all.size());
        for (const core::OwnKvm& kvm : all) {
            rows.append(rowOf(kvm, alike, foundWithout));
        }
    }
    // The same KVMs in the same order as the rows that are there: each row as its KVM is now, in
    // its place, and none made anew. (A row that has the keyboard keeps it.)
    if (std::ranges::equal(rows, m_rows, {}, &Row::id, &Row::id)) {
        for (qsizetype at = 0; at < rows.size(); ++at) {
            if (rows.at(at) != m_rows.at(at)) {
                m_rows[at] = rows.at(at);
                const QModelIndex changed = index(static_cast<int>(at), 0);
                emit dataChanged(changed, changed);
            }
        }
        return;
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged();
}

void OwnKvmsView::follow()
{
    if (m_wall == nullptr) {
        return;
    }
    const QList<core::OwnKvm>& all = m_wall->kvms();
    const Alike alike = alikeIn(all);
    const QSet<QString> foundWithout = m_wall->foundWithoutAPassword();
    QHash<QString, qsizetype> onTheWall;
    for (qsizetype at = 0; at < all.size(); ++at) {
        onTheWall.insert(all.at(at).id, at);
    }
    // Each row in its place: as its KVM is now, or taken off.
    QSet<QString> listed;
    for (qsizetype at = 0; at < m_rows.size(); ++at) {
        Row& row = m_rows[at];
        listed.insert(row.id);
        Row now = row;
        if (const auto there = onTheWall.constFind(row.id); there != onTheWall.cend()) {
            now = rowOf(all.at(there.value()), alike, foundWithout);
        } else {
            now.gone = true;
            now.twins.clear();
            now.noPassword = false;
        }
        if (now != row) {
            row = now;
            const QModelIndex changed = index(static_cast<int>(at), 0);
            emit dataChanged(changed, changed);
        }
    }
    // And what was added, last.
    for (const core::OwnKvm& kvm : all) {
        if (listed.contains(kvm.id)) {
            continue;
        }
        const int at = count();
        beginInsertRows({}, at, at);
        m_rows.append(rowOf(kvm, alike, foundWithout));
        endInsertRows();
        emit countChanged();
    }
}

} // namespace qkvm::ui
