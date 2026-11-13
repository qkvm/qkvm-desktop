#include <qkvm/core/awake_line.h>

#include <algorithm>
#include <utility>

namespace qkvm::core {

namespace {

// The most that may wait to go back to the KVM: nothing goes but answers to its "are you there", and kvmd's own.
constexpr qint64 mostUnsent = qint64{64} * 1024;

// kvmd's own "are you there" on its line for events (the web's EVENT_PING), answered with a `pong`.
constexpr const char* kvmdPing = R"({"event_type": "ping", "event": {}})";

DeviceLine::Limits heldTo(int letInWithinMs)
{
    DeviceLine::Limits limits;
    limits.letInWithinMs = letInWithinMs;
    limits.mostUnsentBytes = mostUnsent;
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
