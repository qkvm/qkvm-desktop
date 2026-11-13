#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <qkvm/core/device_line.h>

namespace qkvm::core {

// A line to a KVM that is held: a WebSocket on which what comes is thrown away. kvmd rests its video while nobody is
// connected, and one such line is somebody (SPEC.md section 6.3).
//
// The line itself is DeviceLine, which says why the program speaks WebSocket itself, how it looks at the KVM's
// certificate before the pass goes out, and what it holds a KVM to. Of what that can do, this hands on the opening and
// the end; and, where it is asked to hear what the KVM says of itself there (hearWhatItSays: kvmd's line for events,
// on which its power board says its state at every change), the short texts that come, and that something came. Then
// it asks the KVM every so often whether it is there, in kvmd's own words: nothing else is ever sent on it but the
// asking for it and the answer to an "are you there".
class AwakeLine : public QObject {
    Q_OBJECT

public:
    using Where = DeviceLine::Where;

    AwakeLine(Where where, int letInWithinMs, QObject* parent = nullptr);

    // Before open: short texts (DeviceLine::hearShortTexts, `mostBytes` at the most) are handed on (`said`), and once
    // the KVM has let it in, it is asked every `pingEveryMs` whether it is there, as kvmd's own page asks it
    // ({"event_type": "ping", "event": {}}, which kvmd answers "pong"; a real Comet within milliseconds).
    void hearWhatItSays(qint64 mostBytes, int pingEveryMs);
    // Once: a second call does nothing.
    void open() { m_line.open(); }
    // The KVM has let it in.
    bool isIn() const { return m_line.isIn(); }

signals:
    // Once: it was not let in, it fell, or the KVM sent what a KVM has no reason to send.
    void ended();
    // A short text the KVM said on it (hearWhatItSays), as it came.
    void said(const QByteArray& text);
    // Something came on it, whatever it was.
    void heard();

private:
    DeviceLine m_line;
    QTimer m_ping;
    int m_pingEveryMs = 0;
};

} // namespace qkvm::core
