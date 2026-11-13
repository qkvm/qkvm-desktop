#include "wall_machines.h"

#include <algorithm>

namespace qkvm::ui {

OnePlace::OnePlace(core::WallSource& place, core::WallFeed& feed, QObject* parent)
    : WallMachines(parent), m_place(&place), m_feed(feed)
{
    connect(&m_feed, &core::WallFeed::wallChanged, this, &WallMachines::wallChanged);
    connect(&m_feed, &core::WallFeed::statusChanged, this, &WallMachines::statusChanged);
}

void OnePlace::setPlace(core::WallSource& place)
{
    // (Never the feed one place's and the keys another's.)
    if (m_feed.isRunning()) {
        return;
    }
    // The feed first, as it was given the next place before anybody else was when each held its
    // own: what it tells of the wall that is gone is told while this still names the place before.
    m_feed.setSource(place);
    m_place = &place;
}

Reached OnePlace::reach(const QString& key) const
{
    const QList<core::Machine>& machines = m_feed.machines();
    if (std::ranges::none_of(machines, [&](const core::Machine& one) { return one.id == key; })) {
        return {};
    }
    return {m_place, key, {}};
}

void OnePlace::lookAgainSoon(const QString& /*key*/)
{
    // (One feed: however many of its machines something was done to, that is one round of looks.)
    m_feed.lookAgainSoon();
}

} // namespace qkvm::ui
