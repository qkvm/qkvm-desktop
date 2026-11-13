#include <qkvm/core/board_holds.h>

#include <algorithm>
#include <utility>

#include <QElapsedTimer>

using namespace Qt::StringLiterals;

namespace qkvm::core {

BoardHolds::BoardHolds(qsizetype most, std::function<qint64()> clockMs)
    : m_most(std::max<qsizetype>(1, most)), m_clockMs(std::move(clockMs))
{}

BoardHolds::~BoardHolds() = default;

BoardHolds& BoardHolds::program()
{
    static BoardHolds kept;
    return kept;
}

qint64 BoardHolds::now() const
{
    if (m_clockMs) {
        return m_clockMs();
    }
    static const QElapsedTimer clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    return clock.elapsed();
}

QString BoardHolds::byAddress(const Of& kvm)
{
    return u"a "_s + kvm.address.toString(QUrl::FullyEncoded);
}

QString BoardHolds::byCertificate(const Of& kvm)
{
    return kvm.certificate.isEmpty() ? QString() : u"c "_s + QString::fromLatin1(kvm.certificate.toHex());
}

std::shared_ptr<BoardHolds::Hold> BoardHolds::find(const Of& kvm) const
{
    const QString mark = byCertificate(kvm);
    std::shared_ptr<Hold> mine = mark.isEmpty() ? nullptr : m_kept.value(mark);
    std::shared_ptr<Hold> there = m_kept.value(byAddress(kvm));
    if (mine != nullptr && there != nullptr && mine != there) {
        // (One KVM, found under both: the longer hold of the two stands, as `of` would make it.)
        return mine->untilMs >= there->untilMs ? mine : there;
    }
    return mine != nullptr ? mine : there;
}

std::shared_ptr<BoardHolds::Hold> BoardHolds::of(const Of& kvm)
{
    const QString mark = byCertificate(kvm);
    const QString address = byAddress(kvm);
    std::shared_ptr<Hold> mine = mark.isEmpty() ? nullptr : m_kept.value(mark);
    const std::shared_ptr<Hold> there = m_kept.value(address);
    if (mine == nullptr && there == nullptr) {
        makeRoom();
        mine = std::make_shared<Hold>();
    } else if (mine == nullptr) {
        mine = there;
    } else if (there != nullptr && there != mine) {
        // The KVM's certificate shown at an address another card had: one KVM there, and what held either holds it.
        if (there->untilMs > mine->untilMs) {
            mine->untilMs = there->untilMs;
            mine->unclear = there->unclear;
            mine->by = there->by;
        }
        mine->out = std::max(mine->out, there->out);
        mine->outUntilMs = std::max(mine->outUntilMs, there->outUntilMs);
        // (And the newer of what the KVM said of its Fingerbot, and of its battery, as the web's Paced is joined.)
        if (there->bot.saidAt > mine->bot.saidAt) {
            mine->bot.paired = there->bot.paired;
            mine->bot.saidAt = there->bot.saidAt;
        }
        if (there->bot.batteryAskedAt > mine->bot.batteryAskedAt) {
            mine->bot.battery = there->bot.battery;
            mine->bot.batteryNext = there->bot.batteryNext;
            mine->bot.batteryAskedAt = there->bot.batteryAskedAt;
        }
        mine->bot.silentAt = std::max(mine->bot.silentAt, there->bot.silentAt);
        mine->bot.askedUntil = std::max(mine->bot.askedUntil, there->bot.askedUntil);
        for (auto kept = m_kept.begin(); kept != m_kept.end(); ++kept) {
            if (kept.value() == there) {
                kept.value() = mine;
            }
        }
    }
    if (!mark.isEmpty()) {
        m_kept.insert(mark, mine);
    }
    m_kept.insert(address, mine);
    return mine;
}

void BoardHolds::makeRoom()
{
    if (m_kept.size() < m_most) {
        return;
    }
    const qint64 at = now();
    m_kept.removeIf([at](const auto& kept) { return kept.value()->idle(at); });
}

bool BoardHolds::Hold::idle(qint64 at) const
{
    // (A "none" said, or nothing heard when it was asked, stands for minutes: kept for ten.)
    constexpr qint64 wordKeptMs = 600'000;
    return untilMs <= at && out == 0 && !bot.paired && !bot.battery && bot.batteryNext <= at && bot.askedUntil <= at &&
           at - bot.silentAt >= wordKeptMs;
}

namespace {

// How long it is held from `at`: for its time; and where a click is out still past that, for as long as its answer is
// on its way (twice its time at the most), or a press reserved it, for as long as its holder's road runs
// (reservedMostMs at the most), said a second at a time.
qint64 leftOf(qint64 untilMs, quint64 out, qint64 outUntilMs, qint64 at)
{
    if (untilMs > at) {
        return untilMs - at;
    }
    return out != 0 && outUntilMs > at ? std::min<qint64>(1000, outUntilMs - at) : 0;
}

} // namespace

qint64 BoardHolds::heldMs(const Of& kvm) const
{
    const std::shared_ptr<Hold> hold = find(kvm);
    return hold != nullptr ? leftOf(hold->untilMs, hold->out, hold->outUntilMs, now()) : 0;
}

bool BoardHolds::unclear(const Of& kvm) const
{
    const std::shared_ptr<Hold> hold = find(kvm);
    return hold != nullptr && leftOf(hold->untilMs, hold->out, hold->outUntilMs, now()) > 0 && hold->unclear;
}

bool BoardHolds::pressing(const Of& kvm) const
{
    const std::shared_ptr<Hold> hold = find(kvm);
    return hold != nullptr && hold->out != 0 && leftOf(hold->untilMs, hold->out, hold->outUntilMs, now()) > 0;
}

bool BoardHolds::stillOut(const Of& kvm, quint64 press) const
{
    const std::shared_ptr<Hold> hold = find(kvm);
    return hold != nullptr && press != 0 && hold->out == press;
}

quint64 BoardHolds::out(const Of& kvm, qint64 forMs, By by)
{
    const std::shared_ptr<Hold> hold = of(kvm);
    // (The same press sent a second time, the KVM having forgotten the session, is still that press: what held it
    // before the first went out is what settles back.)
    if (hold->out == 0) {
        hold->settledUntilMs = hold->untilMs;
        hold->settledUnclear = hold->unclear;
    }
    hold->out = ++m_clicks;
    // (Whose it is, while it is out, heldBy says from this: its answer, or none, settles `by`.)
    hold->outBy = by;
    hold->untilMs = std::max(hold->untilMs, now() + forMs);
    hold->outUntilMs = now() + 2 * forMs;
    hold->unclear = true;
    return hold->out;
}

quint64 BoardHolds::reserve(const Of& kvm, By by)
{
    const std::shared_ptr<Hold> hold = of(kvm);
    if (hold->out == 0) {
        hold->settledUntilMs = hold->untilMs;
        hold->settledUnclear = hold->unclear;
    }
    hold->out = ++m_clicks;
    hold->outBy = by;
    // (Nothing is pressed yet, and nothing of it may be: held while it is the one out, whatever its road takes, and
    // given back by its holder. Only a holder that is lost leaves it to run out.)
    hold->outUntilMs = now() + reservedMostMs;
    hold->unclear = true;
    return hold->out;
}

void BoardHolds::pressed(const Of& kvm, quint64 press, qint64 forMs, By by)
{
    const std::shared_ptr<Hold> hold = of(kvm);
    const qint64 until = now() + forMs;
    if (press != 0 && hold->out == press) {
        hold->out = 0;
        // What held it before this click stands where it is longer than the button's hold.
        hold->unclear = hold->settledUntilMs > until && hold->settledUnclear;
        hold->by = hold->settledUntilMs > until ? hold->by : hold->outBy;
        hold->untilMs = std::max(hold->settledUntilMs, until);
        return;
    }
    if (until > hold->untilMs) {
        hold->untilMs = until;
        hold->unclear = false;
        hold->by = by;
    }
}

void BoardHolds::notPressed(const Of& kvm, quint64 press)
{
    const std::shared_ptr<Hold> hold = find(kvm);
    if (hold == nullptr || press == 0 || hold->out != press) {
        return;
    }
    hold->out = 0;
    hold->untilMs = hold->settledUntilMs;
    hold->unclear = hold->settledUnclear;
}

void BoardHolds::mayBePressing(const Of& kvm, qint64 forMs, By by)
{
    const std::shared_ptr<Hold> hold = of(kvm);
    hold->out = 0;
    if (now() + forMs >= hold->untilMs) {
        hold->by = by;
    }
    hold->untilMs = std::max(hold->untilMs, now() + forMs);
    hold->unclear = true;
}

BoardHolds::By BoardHolds::heldBy(const Of& kvm) const
{
    const std::shared_ptr<Hold> hold = find(kvm);
    if (hold == nullptr || leftOf(hold->untilMs, hold->out, hold->outUntilMs, now()) <= 0) {
        return By::board;
    }
    return hold->out != 0 ? hold->outBy : hold->by;
}

BoardHolds::Fingerbot BoardHolds::fingerbot(const Of& kvm) const
{
    const std::shared_ptr<Hold> hold = find(kvm);
    return hold != nullptr ? hold->bot : Fingerbot{};
}

void BoardHolds::keepFingerbot(const Of& kvm, const Fingerbot& said)
{
    of(kvm)->bot = said;
}

} // namespace qkvm::core
