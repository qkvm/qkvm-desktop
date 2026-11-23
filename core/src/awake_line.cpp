#include <qkvm/core/awake_line.h>

#include <algorithm>
#include <utility>

namespace qkvm::core {

namespace {

// The most that may wait to go back to the KVM: nothing goes but answers to its "are you there", and kvmd's own.
constexpr qint64 mostUnsent = qint64{64} * 1024;

// kvmd's own "are you there" on its line for events (the web's EVENT_PING), answered with a `pong`.
constexpr const char* kvmdPing = R"({"event_type": "ping", "event": {}})";

// The longest message it takes from a KVM, the rest of it thrown away unread where it is longer than what is heard
// (hearWhatItSays), and the most it takes in a second: kvmd says the state of each of its parts as the line opens, the
// state of its drive with every image of its storage named in it among them (the web's MOST_EVENT: kvmd_media.h).
constexpr qint64 mostMessage = qint64{1} << 20;
constexpr qint64 mostASecond = qint64{4} << 20;

DeviceLine::Limits heldTo(int letInWithinMs)
{
    DeviceLine::Limits limits;
    limits.letInWithinMs = letInWithinMs;
    limits.mostUnsentBytes = mostUnsent;
    limits.largestFrameBytes = mostMessage;
    limits.mostBytesASecond = mostASecond;
    return limits;
}

} // namespace

AwakeLine::AwakeLine(Where where, int letInWithinMs, QObject* parent)
    : QObject(parent), m_line(std::move(where), heldTo(letInWithinMs))
{
    connect(&m_line, &DeviceLine::ended, this, [this] {
        m_ping.stop();
        emit ended();
    });
    connect(&m_line, &DeviceLine::textCame, this, &AwakeLine::said);
    connect(&m_line, &DeviceLine::heard, this, &AwakeLine::heard);
    connect(&m_line, &DeviceLine::letIn, this, [this] {
        if (m_pingEveryMs > 0) {
            m_ping.start(m_pingEveryMs);
        }
    });
    // (A line that ends for what was said ends from inside this: told as any end is.)
    connect(&m_ping, &QTimer::timeout, this, [this] { static_cast<void>(m_line.say(QByteArray(kvmdPing))); });
}

void AwakeLine::hearWhatItSays(qint64 mostBytes, int pingEveryMs)
{
    m_line.hearShortTexts(mostBytes);
    m_pingEveryMs = std::max(0, pingEveryMs);
}

} // namespace qkvm::core
