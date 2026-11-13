#include "wall_news.h"

#include <algorithm>

#include <QCoreApplication>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

// Windows shows about 250 letters under a heading; a list of names ends well before that, and
// two lists share the room.
constexpr qsizetype roomForNames = 200;

// A heading has room for 63 letters, and a machine's name can be longer than that.
constexpr qsizetype longestNameInHeading = 40;

QString words(const char* text)
{
    return QCoreApplication::translate("News", text);
}

QString inHeading(const QString& name)
{
    return name.size() > longestNameInHeading ? name.left(longestNameInHeading) + u'…' : name;
}

// The names with commas between them: as many as there is room for, and then how many more.
QString named(const QList<Told>& machines, qsizetype room = roomForNames)
{
    QString names;
    for (qsizetype at = 0; at < machines.size(); ++at) {
        const QString next = (at == 0 ? QString() : u", "_s) + machines.at(at).name;
        if (at > 0 && names.size() + next.size() > room) {
            return names + words(" and %1 more").arg(machines.size() - at);
        }
        names += next;
    }
    return names;
}

void take(QList<Told>& from, const QString& key)
{
    from.removeIf([&](const Told& told) { return told.key == key; });
}

bool has(const QList<Told>& list, const QString& key)
{
    return std::ranges::any_of(list, [&](const Told& told) { return told.key == key; });
}

// One side of the news put to what is there: what is on the other side so far is cancelled by
// it, and anything else is added, once.
void put(QList<Told>& side, QList<Told>& other, const QList<Told>& later)
{
    for (const Told& told : later) {
        if (has(other, told.key)) {
            take(other, told.key);
        } else if (!has(side, told.key)) {
            side.append(told);
        }
    }
}

} // namespace

void remember(WallLook& known, const WallModel& now)
{
    WallLook still;
    for (int row = 0; row < now.rowCount(); ++row) {
        const QModelIndex machine = now.index(row, 0);
        const QString key = machine.data(WallModel::KeyRole).toString();
        const QString as = WallModel::countedAs(machine.data(WallModel::StateRole).toString());
        if (!as.isEmpty()) {
            still.insert(
                key, {as, machine.data(WallModel::NameRole).toString(), machine.data(WallModel::LabelRole).toString()});
        } else if (const auto before = known.constFind(key); before != known.constEnd()) {
            still.insert(key, *before);
        }
    }
    known = still;
}

WallNews newsBetween(const WallLook& known, const WallModel& now)
{
    WallNews news;
    for (int row = 0; row < now.rowCount(); ++row) {
        const QModelIndex machine = now.index(row, 0);
        const QString key = machine.data(WallModel::KeyRole).toString();
        const QString as = WallModel::countedAs(machine.data(WallModel::StateRole).toString());
        const auto was = known.constFind(key);
        // Between two times at which it was known to be some way, and between no others.
        if (as.isEmpty() || was == known.constEnd()) {
            continue;
        }
        const bool wantedAttention = was->as == "attention"_L1;
        const bool wantsAttention = as == "attention"_L1;
        if (wantedAttention != wantsAttention) {
            (wantsAttention ? news.away : news.back)
                .append(
                    {key, machine.data(WallModel::NameRole).toString(), machine.data(WallModel::LabelRole).toString()});
        }
    }
    return news;
}

void add(WallNews& soFar, const WallNews& later)
{
    put(soFar.away, soFar.back, later.away);
    put(soFar.back, soFar.away, later.back);
}

std::optional<Message> messageOf(const WallNews& news)
{
    if (news.isEmpty()) {
        return std::nullopt;
    }
    if (news.back.isEmpty()) {
        return news.away.size() == 1
                   ? Message{words("%1 needs attention").arg(inHeading(news.away.constFirst().name)),
                             news.away.constFirst().label, true}
                   : Message{words("%1 KVMs need attention").arg(news.away.size()), named(news.away), true};
    }
    if (news.away.isEmpty()) {
        return news.back.size() == 1
                   ? Message{words("%1 is back").arg(inHeading(news.back.constFirst().name)),
                             news.back.constFirst().label, false}
                   : Message{words("%1 KVMs are back").arg(news.back.size()), named(news.back), false};
    }
    return Message{words("%1 need attention, %2 back").arg(news.away.size()).arg(news.back.size()),
                   words("Need attention: %1. Back: %2.")
                       .arg(named(news.away, roomForNames / 2), named(news.back, roomForNames / 2)),
                   true};
}

} // namespace qkvm::ui
