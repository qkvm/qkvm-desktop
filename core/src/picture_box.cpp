#include <qkvm/core/picture_box.h>

#include <utility>

namespace qkvm::core {
namespace {
// The largest RTP packet the library hands over that is put together (its own largest message).
constexpr qsizetype largestPacket = 65536;
} // namespace

PictureBox::PictureBox(int payloadType, Pace pace) : m_assembler(payloadType), m_pace(pace) {}

bool PictureBox::parameters(QByteArrayView sps, QByteArrayView pps)
{
    return m_assembler.parameters(sps, pps);
}

bool PictureBox::packet(QByteArrayView rtp, qint64 nowUs)
{
    if (rtp.isEmpty() || rtp.size() > largestPacket) {
        m_assembler.lose();
        return !m_waiting.isEmpty();
    }
    std::optional<H264Rtp::Picture> whole = m_assembler.take(rtp);
    if (!whole) {
        return !m_waiting.isEmpty();
    }
    ++m_counts.whole;
    m_lastWholeUs = nowUs;
    if (m_untilNextKey && !whole->key) {
        return !m_waiting.isEmpty();
    }
    m_untilNextKey = false;
    if (whole->key) {
        // What a window wished for has come.
        m_wished = false;
    }
    if (m_cardOnly && !whole->key) {
        ++m_counts.letGoAsCardOnly;
        return !m_waiting.isEmpty();
    }
    keep({std::move(whole->bytes), whole->key, nowUs});
    return !m_waiting.isEmpty();
}

void PictureBox::keep(Picture picture)
{
    if (m_waiting.size() < m_pace.most) {
        m_waiting.append(std::move(picture));
        return;
    }
    // One more than may wait. A key frame stands by itself; otherwise the run from the newest key frame
    // held, with this one, where that is no longer than the most (what is before that key frame is of
    // no use to the decoder any more); otherwise all goes, and the decoder waits for the next key frame.
    if (picture.key) {
        m_counts.letGoBehind += static_cast<quint64>(m_waiting.size());
        m_waiting.clear();
        m_waiting.append(std::move(picture));
        return;
    }
    qsizetype newestKey = -1;
    for (qsizetype at = m_waiting.size() - 1; at >= 0; --at) {
        if (m_waiting[at].key) {
            newestKey = at;
            break;
        }
    }
    if (newestKey > 0 && m_waiting.size() - newestKey + 1 <= m_pace.most) {
        m_counts.letGoBehind += static_cast<quint64>(newestKey);
        m_waiting.remove(0, newestKey);
        m_waiting.append(std::move(picture));
        return;
    }
    // (The one that came goes too.)
    m_counts.letGoBehind += static_cast<quint64>(m_waiting.size()) + 1;
    m_waiting.clear();
    m_assembler.lose();
    ++m_counts.runsLost;
}

void PictureBox::keyFramesOnly(bool only)
{
    // Back to every picture: what was let go meanwhile is what the next ones build on, so they are kept
    // from the next key frame on.
    if (m_cardOnly && !only) {
        m_untilNextKey = true;
    }
    m_cardOnly = only;
}

void PictureBox::fromTheNextKeyFrame()
{
    m_waiting.clear();
    m_untilNextKey = true;
}

bool PictureBox::asks(qint64 nowUs)
{
    if (m_asked && nowUs - m_askedAtUs < qint64{m_pace.keyAskEveryMs} * 1000) {
        return false;
    }
    m_asked = true;
    m_askedAtUs = nowUs;
    m_wished = false;
    ++m_counts.keyAsks;
    return true;
}

bool PictureBox::lostAndAsks(qint64 nowUs)
{
    return (m_assembler.needsKey() || m_wished || m_untilNextKey) && asks(nowUs);
}

bool PictureBox::mayAsk(qint64 nowUs)
{
    if (asks(nowUs)) {
        return true;
    }
    // Not forgotten: asked once the pace has passed (lostAndAsks), unless a key frame comes first.
    m_wished = true;
    return false;
}

bool PictureBox::wishAsks(qint64 nowUs)
{
    return m_wished && asks(nowUs);
}

qint64 PictureBox::paceLeftUs(qint64 nowUs) const
{
    if (!m_asked) {
        return 0;
    }
    const qint64 left = m_askedAtUs + qint64{m_pace.keyAskEveryMs} * 1000 - nowUs;
    return left > 0 ? left : 0;
}

QList<PictureBox::Picture> PictureBox::take()
{
    m_counts.handed += static_cast<quint64>(m_waiting.size());
    return std::exchange(m_waiting, {});
}

} // namespace qkvm::core
