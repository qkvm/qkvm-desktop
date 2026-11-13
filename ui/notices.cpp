#include "notices.h"

#include <algorithm>
#include <utility>

#include <QTimer>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

Notices::Notices(QObject* parent, int stayPercent) : QAbstractListModel(parent), m_stayPercent(stayPercent) {}

int Notices::staysMs(Kind kind, const QString& text)
{
    const int least = kind == Kind::Failed ? 8000 : 4000;
    return std::min(12000, std::max(least, static_cast<int>(std::min<qsizetype>(text.size(), 1000)) * 70));
}

int Notices::add(Kind kind, const QString& text, bool waits)
{
    // Room for it: the oldest goes that is not the only sign of something under way, nor the
    // only word of something that nobody has seen yet.
    while (m_notices.size() >= most) {
        const auto oldest =
            std::ranges::find_if(m_notices, [](const Notice& one) { return one.kind != Kind::UnderWay && !one.waits; });
        if (oldest == m_notices.end()) {
            break;
        }
        remove(oldest->number);
    }
    const int row = static_cast<int>(m_notices.size());
    beginInsertRows({}, row, row);
    m_notices.append(Notice{++m_last, kind, text, waits});
    endInsertRows();
    emit countChanged();
    return m_last;
}

void Notices::goLater(int number, Kind kind, const QString& text)
{
    QTimer::singleShot(staysMs(kind, text) * m_stayPercent / 100, this, [this, number] { remove(number); });
}

void Notices::tell(Kind kind, const QString& text)
{
    if (kind == Kind::UnderWay) {
        // One that nobody could take away again would stay for good.
        kind = Kind::Info;
    }
    goLater(add(kind, text), kind, text);
}

void Notices::tellWhenSeen(Kind kind, const QString& text)
{
    if (m_seen || kind == Kind::UnderWay) {
        tell(kind, text);
        return;
    }
    add(kind, text, true);
}

void Notices::setSeen(bool seen)
{
    if (seen == m_seen) {
        return;
    }
    m_seen = seen;
    if (!m_seen) {
        return;
    }
    // What waited for somebody to see it has its time from here.
    for (Notice& one : m_notices) {
        if (std::exchange(one.waits, false)) {
            goLater(one.number, one.kind, one.text);
        }
    }
}

QList<Notices::Told> Notices::takeWhatWaits()
{
    QList<Told> waited;
    QList<int> numbers;
    for (const Notice& one : std::as_const(m_notices)) {
        if (one.waits) {
            waited.append(Told{one.kind, one.text});
            numbers.append(one.number);
        }
    }
    for (const int number : std::as_const(numbers)) {
        remove(number);
    }
    return waited;
}

int Notices::underWay(const QString& text)
{
    return add(Kind::UnderWay, text);
}

void Notices::over(int number)
{
    remove(number);
}

void Notices::remove(int number)
{
    const auto found = std::ranges::find_if(m_notices, [number](const Notice& one) { return one.number == number; });
    if (found == m_notices.end()) {
        return;
    }
    const int row = static_cast<int>(found - m_notices.begin());
    beginRemoveRows({}, row, row);
    m_notices.erase(found);
    endRemoveRows();
    emit countChanged();
}

void Notices::clear()
{
    if (m_notices.isEmpty()) {
        return;
    }
    beginResetModel();
    m_notices.clear();
    endResetModel();
    emit countChanged();
}

int Notices::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_notices.size());
}

QVariant Notices::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_notices.size()) {
        return {};
    }
    const Notice& notice = m_notices[index.row()];
    switch (role) {
    case TextRole:
        return notice.text;
    case KindRole:
        switch (notice.kind) {
        case Kind::Done:
            return u"ok"_s;
        case Kind::Failed:
            return u"error"_s;
        case Kind::UnderWay:
            return u"working"_s;
        case Kind::Info:
            break;
        }
        return u"info"_s;
    case NumberRole:
        return notice.number;
    default:
        return {};
    }
}

QHash<int, QByteArray> Notices::roleNames() const
{
    return {{TextRole, "text"}, {KindRole, "kind"}, {NumberRole, "number"}};
}

} // namespace qkvm::ui
