#include "wall_model.h"

#include <algorithm>

#include <QCollator>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// Names in the order a person expects: "Server 2" before "Server 10", and capitals no matter.
int compared(const QString& one, const QString& other)
{
    static const QCollator collator = [] {
        QCollator made;
        made.setNumericMode(true);
        made.setCaseSensitivity(Qt::CaseInsensitive);
        return made;
    }();
    return collator.compare(one, other);
}

// What needs attention first, then what gives no picture, then what is simply there; a state that
// is none of these (a machine still connecting) after them.
int rankOf(const QString& state)
{
    const QString counted = WallModel::countedAs(state);
    return counted == "attention"_L1 ? 0 : counted == "no_signal"_L1 ? 1 : counted == "online"_L1 ? 2 : 3;
}

} // namespace

WallModel::WallModel(QObject* parent) : QAbstractListModel(parent) {}

void WallModel::setTiles(QList<WallTile> tiles)
{
    const bool sameMachines =
        tiles.size() == m_tiles.size() && std::ranges::equal(tiles, m_tiles, {}, &WallTile::key, &WallTile::key);
    if (!sameMachines) {
        beginResetModel();
        m_tiles = std::move(tiles);
        // A machine that left the wall is picked no longer, nor one that may not be acted on.
        const qsizetype pickedBefore = m_picked.size();
        QSet<QString> there;
        for (const WallTile& tile : std::as_const(m_tiles)) {
            if (tile.actable) {
                there.insert(tile.key);
            }
        }
        m_picked.intersect(there);
        endResetModel();
        emit countsChanged();
        lookForGroups();
        if (m_picked.size() != pickedBefore) {
            emit pickedChanged();
        }
        return;
    }
    bool changed = false;
    bool unpicked = false;
    for (qsizetype row = 0; row < tiles.size(); ++row) {
        if (tiles[row] != m_tiles[row]) {
            m_tiles[row] = std::move(tiles[row]);
            // (One that may not be acted on any more is picked no longer.)
            if (!m_tiles[row].actable && m_picked.remove(m_tiles[row].key)) {
                unpicked = true;
            }
            emit dataChanged(index(static_cast<int>(row)), index(static_cast<int>(row)));
            changed = true;
        }
    }
    if (changed) {
        emit countsChanged();
        lookForGroups();
    }
    if (unpicked) {
        emit pickedChanged();
    }
}

QStringList WallModel::picked() const
{
    QStringList keys;
    for (const WallTile& tile : std::as_const(m_tiles)) {
        if (m_picked.contains(tile.key)) {
            keys.append(tile.key);
        }
    }
    return keys;
}

void WallModel::setPicked(const QString& key, bool picked)
{
    for (qsizetype row = 0; row < m_tiles.size(); ++row) {
        if (m_tiles[row].key != key) {
            continue;
        }
        if (picked == m_picked.contains(key) || (picked && !m_tiles[row].actable)) {
            return;
        }
        if (picked) {
            m_picked.insert(key);
        } else {
            m_picked.remove(key);
        }
        emit dataChanged(index(static_cast<int>(row)), index(static_cast<int>(row)), {PickedRole});
        emit pickedChanged();
        return;
    }
}

void WallModel::pick(const QStringList& keys)
{
    for (const QString& key : keys) {
        setPicked(key, true);
    }
}

void WallModel::pickNone()
{
    if (m_picked.isEmpty()) {
        return;
    }
    m_picked.clear();
    if (!m_tiles.isEmpty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_tiles.size()) - 1), {PickedRole});
    }
    emit pickedChanged();
}

void WallModel::lookForGroups()
{
    QStringList groups;
    for (const WallTile& tile : std::as_const(m_tiles)) {
        if (!tile.group.isEmpty() && !groups.contains(tile.group)) {
            groups.append(tile.group);
        }
    }
    std::ranges::sort(groups, [](const QString& one, const QString& other) { return compared(one, other) < 0; });
    if (groups != m_groups) {
        m_groups = groups;
        emit groupsChanged();
    }
}

QString WallModel::countedAs(const QString& state)
{
    if (state == "online"_L1) {
        return u"online"_s;
    }
    if (state == "no_signal"_L1) {
        return u"no_signal"_s;
    }
    if (state == "offline"_L1 || state == "auth"_L1 || state == "error"_L1 || state == "busy"_L1) {
        return u"attention"_s;
    }
    return {};
}

int WallModel::counted(const QString& as) const
{
    return static_cast<int>(
        std::ranges::count_if(m_tiles, [&](const WallTile& tile) { return countedAs(tile.state) == as; }));
}

int WallModel::all() const
{
    return static_cast<int>(m_tiles.size());
}

int WallModel::online() const
{
    return counted(u"online"_s);
}

int WallModel::noSignal() const
{
    return counted(u"no_signal"_s);
}

int WallModel::attention() const
{
    return counted(u"attention"_s);
}

int WallModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tiles.size());
}

QVariant WallModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tiles.size()) {
        return {};
    }
    const WallTile& tile = m_tiles.at(index.row());
    switch (role) {
    case StreamRole:
        return tile.stream;
    case NameRole:
        return tile.name;
    case HostRole:
        return tile.host;
    case StateRole:
        return tile.state;
    case TitleRole:
        return tile.title;
    case TextRole:
        return tile.text;
    case ActionRole:
        return tile.action;
    case MetaRole:
        return tile.meta;
    case PortsRole:
        return tile.ports;
    case ServerRole:
        return tile.server;
    case KeyRole:
        return tile.key;
    case LabelRole:
        return tile.label;
    case GroupRole:
        return tile.group;
    case OpenableRole:
        return tile.openable;
    case PickedRole:
        return m_picked.contains(tile.key);
    case SourceRole:
        return tile.source;
    case AwayRole:
        return tile.away;
    case ActableRole:
        return tile.actable;
    case NoteRole:
        return tile.note;
    case LargeMetaRole:
        return tile.largeMeta;
    case AmtViewRole:
        return tile.amtView;
    case PictureSizeRole:
        return tile.pictureSize;
    default:
        return {};
    }
}

QHash<int, QByteArray> WallModel::roleNames() const
{
    return {
        {StreamRole, "stream"_ba},   {NameRole, "name"_ba},
        {HostRole, "host"_ba},       {StateRole, "status"_ba},
        {TitleRole, "title"_ba},     {TextRole, "text"_ba},
        {ActionRole, "action"_ba},   {MetaRole, "meta"_ba},
        {PortsRole, "ports"_ba},     {ServerRole, "server"_ba},
        {KeyRole, "key"_ba},         {LabelRole, "label"_ba},
        {GroupRole, "group"_ba},     {OpenableRole, "openable"_ba},
        {PickedRole, "picked"_ba},   {SourceRole, "source"_ba},
        {AwayRole, "away"_ba},       {ActableRole, "actable"_ba},
        {NoteRole, "note"_ba},       {LargeMetaRole, "largeMeta"_ba},
        {AmtViewRole, "amtView"_ba}, {PictureSizeRole, "pictureSize"_ba},
    };
}

WallFilter::WallFilter(QObject* parent) : QSortFilterProxyModel(parent), m_show(u"all"_s)
{
    // An order that depends on how the machines are follows them as they change.
    setDynamicSortFilter(true);
    // Whatever changes how many rows are shown is one signal for the screen.
    connect(this, &QAbstractItemModel::rowsInserted, this, &WallFilter::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &WallFilter::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &WallFilter::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &WallFilter::countChanged);
}

QString WallFilter::show() const
{
    return m_show;
}

void WallFilter::setShow(const QString& show)
{
    if (show == m_show) {
        return;
    }
    beginFilterChange();
    m_show = show;
    endFilterChange();
    emit showChanged();
    emit countChanged();
}

QString WallFilter::find() const
{
    return m_find;
}

void WallFilter::setFind(const QString& find)
{
    if (find == m_find) {
        return;
    }
    beginFilterChange();
    m_find = find;
    endFilterChange();
    emit findChanged();
    emit countChanged();
}

void WallFilter::setGroup(const QString& group)
{
    if (group == m_group) {
        return;
    }
    beginFilterChange();
    m_group = group;
    endFilterChange();
    emit groupChanged();
    emit countChanged();
}

void WallFilter::lookAgain()
{
    beginFilterChange();
    endFilterChange();
    emit countChanged();
}

void WallFilter::setOrder(const QString& order)
{
    const QString known = order == "name"_L1 || order == "group"_L1 || order == "state"_L1 ? order : QString();
    if (known == m_order) {
        return;
    }
    m_order = known;
    // No column to sort by is the order the machines came in; which is also how they are shown
    // while the wall's own order is being made, whatever was chosen.
    invalidate();
    sort(m_arranging || m_order.isEmpty() ? -1 : 0);
    emit orderChanged();
}

void WallFilter::setArranging(bool arranging, const QString& of)
{
    const QString arrangingOf = arranging ? of : QString();
    if (arranging == m_arranging && arrangingOf == m_arrangingOf) {
        return;
    }
    beginFilterChange();
    m_arranging = arranging;
    m_arrangingOf = arrangingOf;
    endFilterChange();
    // As the machines came while the order is made; and by what the bar chose again afterwards,
    // where no column to sort by is the order the machines came in as well.
    invalidate();
    sort(m_arranging || m_order.isEmpty() ? -1 : 0);
    emit countChanged();
}

QStringList WallFilter::keys() const
{
    QStringList all;
    for (int row = 0; row < rowCount(); ++row) {
        all.append(index(row, 0).data(WallModel::KeyRole).toString());
    }
    return all;
}

bool WallFilter::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const QString leftName = left.data(WallModel::NameRole).toString();
    const QString rightName = right.data(WallModel::NameRole).toString();
    if (m_order == "group"_L1) {
        const QString leftGroup = left.data(WallModel::GroupRole).toString();
        const QString rightGroup = right.data(WallModel::GroupRole).toString();
        // Machines without a group come after the groups.
        if (leftGroup.isEmpty() != rightGroup.isEmpty()) {
            return rightGroup.isEmpty();
        }
        if (const int groups = compared(leftGroup, rightGroup); groups != 0) {
            return groups < 0;
        }
    } else if (m_order == "state"_L1) {
        const int leftRank = rankOf(left.data(WallModel::StateRole).toString());
        const int rightRank = rankOf(right.data(WallModel::StateRole).toString());
        if (leftRank != rightRank) {
            return leftRank < rightRank;
        }
    }
    return compared(leftName, rightName) < 0;
}

int WallFilter::count() const
{
    return rowCount();
}

bool WallFilter::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    const QModelIndex index = sourceModel()->index(row, 0, parent);
    // Nobody arranges what is hidden by a search, or by a choice of the bar's; and an order is of
    // one source's machines, which are shown, and no others.
    if (m_arranging) {
        return index.data(WallModel::SourceRole).toString() == m_arrangingOf;
    }
    if (!m_group.isEmpty() && index.data(WallModel::GroupRole).toString() != m_group) {
        const auto* wall = qobject_cast<const WallModel*>(sourceModel());
        if (wall == nullptr || wall->groups().contains(m_group)) {
            return false;
        }
    }
    if (m_show != "all"_L1 && WallModel::countedAs(index.data(WallModel::StateRole).toString()) != m_show) {
        return false;
    }
    // As the page looks: in the name, the address and the group.
    const QString wanted = m_find.trimmed();
    return wanted.isEmpty() || index.data(WallModel::NameRole).toString().contains(wanted, Qt::CaseInsensitive) ||
           index.data(WallModel::HostRole).toString().contains(wanted, Qt::CaseInsensitive) ||
           index.data(WallModel::GroupRole).toString().contains(wanted, Qt::CaseInsensitive);
}

} // namespace qkvm::ui
