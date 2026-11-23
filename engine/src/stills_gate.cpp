#include "stills_gate.h"

#include <algorithm>

#include <qkvm/base/clock.h>

namespace qkvm::engine {

StillsGate::StillsGate(DecodePool& pool, int atOnce) : m_pool(pool), m_atOnce(atOnce) {}

void StillsGate::handIn(Member& stream, bool fresh)
{
    const std::lock_guard guard(m_lock);
    // A stream that goes hands in nothing more: what its last decoding hands in as it ends would put it back in the
    // line, or in a place, after it was taken out of both, and the line would wake it once freed (the review of
    // abb20deb, M2).
    if (stream.gone) {
        return;
    }
    // No card any more, since the stream looked whether it was one: woken at once, as every stream that is no card
    // is, and never in line (setCard took it out of it; the review of abb20deb, M1).
    if (!stream.card.load(std::memory_order_relaxed)) {
        m_pool.wake(&stream.work);
        return;
    }
    m_seen.handedIn += fresh ? 1 : 0;
    const bool holding = std::ranges::find(m_holding, &stream) != m_holding.end();
    if (!holding) {
        if (std::ranges::any_of(m_line, [&stream](const InLine& one) { return one.stream == &stream; })) {
            // In line already: it is woken when its turn comes, and decodes the newest it has then. (A still that came
            // while it waits took the place of the one that waited: counted, for what is said of it.)
            m_seen.replacedWaiting += fresh ? 1 : 0;
            return;
        }
        if (m_atOnce > 0 && static_cast<int>(m_holding.size()) >= m_atOnce) {
            m_line.push_back({&stream, base::steadyNowUs()});
            ++m_seen.waited;
            return;
        }
        m_holding.push_back(&stream);
    }
    // (Woken with the lock held: a stream that goes is forgotten here first, and the pool after, so nothing wakes
    // it once it is gone.)
    m_pool.wake(&stream.work);
}

bool StillsGate::setCard(Member& stream, bool card)
{
    const std::lock_guard guard(m_lock);
    if (stream.gone || stream.card.load(std::memory_order_relaxed) == card) {
        return false;
    }
    stream.card.store(card, std::memory_order_relaxed);
    // A card that waits in line when its machine comes to be shown large leaves it at once: the still it waited with
    // is woken by itself (StreamCore::shownAsACard), and its whole picture does not wait for the line's turn (the
    // review of abb20deb, M1).
    if (!card) {
        std::erase_if(m_line, [&stream](const InLine& one) { return one.stream == &stream; });
    }
    return true;
}

bool StillsGate::holds(const Member& stream) const
{
    const std::lock_guard guard(m_lock);
    return std::ranges::find(m_holding, &stream) != m_holding.end();
}

void StillsGate::leave(Member& stream)
{
    const std::lock_guard guard(m_lock);
    const auto there = std::ranges::find(m_holding, &stream);
    if (there == m_holding.end()) {
        return;
    }
    m_holding.erase(there);
    wakeTheNext();
}

void StillsGate::forget(Member& stream)
{
    const std::lock_guard guard(m_lock);
    stream.gone = true;
    std::erase_if(m_line, [&stream](const InLine& one) { return one.stream == &stream; });
    if (const auto there = std::ranges::find(m_holding, &stream); there != m_holding.end()) {
        m_holding.erase(there);
        wakeTheNext();
    }
}

void StillsGate::wakeTheNext()
{
    if (m_line.empty() || (m_atOnce > 0 && static_cast<int>(m_holding.size()) >= m_atOnce)) {
        return;
    }
    const InLine next = m_line.front();
    m_line.pop_front();
    m_seen.longestWaitMs =
        std::max(m_seen.longestWaitMs, static_cast<double>(base::steadyNowUs() - next.sinceUs) / 1000.0);
    m_holding.push_back(next.stream);
    m_pool.wake(&next.stream->work);
}

void StillsGate::decodingBegins()
{
    const std::lock_guard guard(m_lock);
    ++m_decoding;
    m_seen.mostAtOnce = std::max(m_seen.mostAtOnce, m_decoding);
}

void StillsGate::decodingEnds()
{
    const std::lock_guard guard(m_lock);
    --m_decoding;
    ++m_seen.decoded;
}

StillsGate::Seen StillsGate::seen()
{
    const std::lock_guard guard(m_lock);
    Seen said = m_seen;
    said.holdingNow = static_cast<int>(m_holding.size());
    said.inLineNow = static_cast<int>(m_line.size());
    // (What is being decoded now counts towards the next asking from where it stands.)
    m_seen = Seen{};
    m_seen.mostAtOnce = m_decoding;
    return said;
}

} // namespace qkvm::engine
