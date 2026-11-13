#include "pc_source.h"

#include <algorithm>

#include <QUrl>

using namespace Qt::StringLiterals;

namespace qkvm::ui {

PcSource::PcSource(core::LocalWall& wall, int statusEveryMs, QObject* parent)
    : Source(parent), m_wall(wall), m_feed(wall, statusEveryMs)
{
    // The wall's KVMs, or their order, are not what they were (a KVM was added, changed or taken
    // off, or the order was made): the wall is asked for anew at once, and not at its next turn;
    // and looked at again soon after, as after something done to a machine: a KVM that was just
    // added is being signed in to.
    connect(&m_wall, &core::LocalWall::changed, this, [this] {
        if (m_atTheWall || (m_monitors && m_feed.isRunning())) {
            m_feed.askAnew();
            m_feed.lookAgainSoon();
            emit kvmsChanged();
        }
    });
    // A KVM came to something with nobody having asked how it is (it was asked what it is and
    // signed in to, or was not found): the machines are looked at now, and its tile says so
    // within a moment and not at the feed's next turn, seconds later.
    connect(&m_wall, &core::LocalWall::cameToSomething, this, [this] {
        if (m_atTheWall || (m_monitors && m_feed.isRunning())) {
            m_feed.lookNow();
        }
    });
}

QString PcSource::idOfThisPc()
{
    return u"pc"_s;
}

void PcSource::enter()
{
    if (m_atTheWall) {
        return;
    }
    m_atTheWall = true;
    emit entered(whoever());
}

void PcSource::leave()
{
    if (!m_atTheWall) {
        return;
    }
    m_atTheWall = false;
    // (Before anybody hears of it: whoever has the windows checks an answer against it.)
    ++m_turn;
    emit left();
    emit emptied();
}

core::Account PcSource::whoever()
{
    core::Account whoever;
    whoever.id = u"this-pc"_s;
    whoever.role = u"admin"_s;
    return whoever;
}

bool PcSource::isEncrypted() const
{
    // What travels to a KVM does so as that KVM was added: one that is spoken to over plain HTTP
    // is enough for the bar to say so.
    return std::ranges::none_of(m_wall.kvms(),
                                [](const core::OwnKvm& kvm) { return QUrl(kvm.address).scheme() == "http"_L1; });
}

} // namespace qkvm::ui
