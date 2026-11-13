#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "live_stand_in.h"

// A server for the tests of the program's own: a hundred lines over a TCP socket that answer what a
// test tells them to, and remember what they were asked (DESIGN.md section 8). On the loopback
// address, at a port Windows chooses. In the clear, or over TLS with a certificate a test names.
namespace qkvm::core::testing {

struct Asked {
    QByteArray method;
    QByteArray path;                       // with what stands behind the question mark
    QHash<QByteArray, QByteArray> headers; // names in small letters
    QByteArray body;
    int line = 0; // the line it came on: counted from 1, in the order the lines came
};

struct Answered {
    // NOLINTNEXTLINE(google-explicit-constructor): a test writes {200, "..."} where an answer is wanted
    Answered(int status = 200, QByteArray body = {}, QByteArray type = "application/json", QList<QByteArray> more = {})
        : status(status), body(std::move(body)), type(std::move(type)), more(std::move(more))
    {}

    int status;
    QByteArray body;
    QByteArray type;
    QList<QByteArray> more; // further header lines, each whole: "Set-Cookie: a=b"
    bool never = false;     // say nothing at all, and keep the line open: a server that does not answer
    bool hangUp = false;    // say nothing and close the line: a server that falls over at this question
    int afterMs = 0;        // answer only after this long: a server that is waiting for a machine
    int bodyAfterMs = 0;    // and the body this much later than what comes before it
    int byteEveryMs = 0;    // send the answer a byte at a time, this far apart, and never all of it
    int shortBy = 0;        // say the answer is this many bytes longer than it is, and close the line
    // An answer that does not end: what comes before the body, with no length to it, then the
    // body, and the line is kept. What follows on it is the test's to send (StandIn::sendMore).
    bool streams = false;
    // Say the answer is this many bytes longer than it is, and keep the line: a server that is
    // still sending. (As an answer that is too large looks to whoever cuts it off. A server that
    // hangs up under such an answer is another matter: Qt then fails, with that answer, every
    // question to the server that still waits for a line of its own, as if the server had hung up
    // on each. Seen.)
    int moreToCome = 0;
};

// A picture of a machine's screen, as far as a stand-in's has to be one: it begins as a JPEG
// does, which is what the program takes for a picture (with the kind "image/jpeg" said of it).
inline QByteArray aPicture()
{
    return QByteArray::fromHex("ffd8ffe0") + "a picture";
}

// A place where no server is: a port that is held for as long as this is there, so that Windows
// gives it to nobody else, and that nothing listens on. (A port some server had a moment ago is
// not that: Windows gives it to whoever asks for a port next, another test program's server
// among them.)
class NobodyThere {
public:
    NobodyThere() { m_held.bind(QHostAddress::LocalHost, 0, QAbstractSocket::DontShareAddress); }

    quint16 port() const { return m_held.localPort(); }
    QString address() const { return QStringLiteral("http://127.0.0.1:%1").arg(port()); }

private:
    QTcpSocket m_held;
};

// A certificate and its key, as tools/make_testdata.ps1 makes them for these tests.
struct Tls {
    QString certificateFile;
    QString keyFile;
};

// A server over TLS that takes up a caller's conversation only after a while: a KVM that is slow
// to let a line in (one behind a VPN or a relay, or one that is busy). The caller is connected
// at once, as Windows connects whoever calls a port that somebody listens on, and nothing is
// agreed on how to speak until the while is over: whoever asks over HTTPS has not been let in
// until then, and sends nothing. (Over plain HTTP a caller is let in by Windows itself, the
// moment it calls: there, how soon a line is let in cannot be made slow on this PC's own
// address.)
class LateToLetIn : public QSslServer {
public:
    int afterMs = 0;

protected:
    void incomingConnection(qintptr socket) override
    {
        if (afterMs <= 0) {
            QSslServer::incomingConnection(socket);
            return;
        }
        // (A caller that has given up meanwhile is found gone when the conversation is taken
        // up, and Qt lets go of it. One that still waits when this server goes stays Windows'
        // until the test program ends.)
        QTimer::singleShot(afterMs, this, [this, socket] { QSslServer::incomingConnection(socket); });
    }
};

class StandIn : public QObject {
public:
    // At a port of the test's choosing where one server is to take the place of another; whether
    // Windows gave it that port is asked with isListening().
    explicit StandIn(std::function<Answered(const Asked&)> answer, quint16 port = 0)
        : m_answer(std::move(answer)), m_server(std::make_unique<QTcpServer>())
    {
        listen(port);
    }

    StandIn(std::function<Answered(const Asked&)> answer, const Tls& tls, quint16 port = 0)
        : m_answer(std::move(answer)), m_encrypted(true)
    {
        auto server = std::make_unique<LateToLetIn>();
        m_late = server.get();
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        // The server's own certificate, and after it whichever a server sends along with it.
        configuration.setLocalCertificateChain(QSslCertificate::fromPath(tls.certificateFile, QSsl::Pem));
        QFile key(tls.keyFile);
        if (key.open(QIODevice::ReadOnly)) {
            configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
        }
        server->setSslConfiguration(configuration);
        m_server = std::move(server);
        listen(port);
    }

    // The server goes first, and its callers with it, while what they are noted in is still there.
    ~StandIn() override { m_server.reset(); }

    static QSslCertificate certificateIn(const QString& file)
    {
        QFile kept(file);
        return kept.open(QIODevice::ReadOnly) ? QSslCertificate(kept.readAll(), QSsl::Pem) : QSslCertificate();
    }

    bool isListening() const { return m_server->isListening(); }
    // Over TLS: from here on a caller is let in only after so long (LateToLetIn, above). Not in
    // the clear, where that cannot be had: there this does nothing, and says so (false).
    bool letInOnlyAfter(int ms)
    {
        if (m_late == nullptr) {
            return false;
        }
        m_late->afterMs = ms;
        return true;
    }
    // From here on nobody new is let in, as when its address has become another's: whoever is on
    // a line already is answered on it as before. (Its port is asked for before this.)
    void takeNoMore() { m_server->close(); }
    quint16 serverPort() const { return m_server->serverPort(); }
    QString address() const
    {
        return QStringLiteral("%1://127.0.0.1:%2")
            .arg(m_encrypted ? QStringLiteral("https") : QStringLiteral("http"))
            .arg(serverPort());
    }

    // The live lines of this server: whoever asks this address to be made one (a WebSocket) is
    // handed to it, as a QKVM has its calls and its live lines at one address. What was asked for
    // and said on such a line is noted there, and not among the calls.
    LiveStandIn live{LiveStandIn::Handed::Over};

    // Everything that was asked, in the order it came.
    QList<Asked> asked;
    // Called after a complete request is recorded, before its answer is queued. Certificate
    // replacement tests can switch listeners at the last probe without racing a wall clock.
    std::function<void()> beforeAnswer;
    // Whether a caller's line is kept for its next question, as a real server keeps it; else it
    // is closed after every answer. And how many lines callers have made.
    bool keepsLines = false;
    int lines = 0;
    // Whether it closes a caller's line right after every answer without saying it would (no "Connection: close",
    // which HTTP/1.1 reads as a line that is kept), as a controller may: whoever asked finds the line gone only once
    // it uses it again.
    bool closesSilently = false;
    // A line it kept that has been idle this long since its last answer is one it is closing (as a web server closes a
    // line kept idle for its keep-alive, uvicorn's five seconds): a question that comes on it then meets the close on
    // the way, is not answered, and the line is gone at once. Counted in `forgotten`. 0: a kept line is kept.
    int forgetsIdleAfterMs = 0;
    int forgotten = 0;
    // Whether whoever asks to be made a live line is handed to `live`; else such an asking is
    // a question like any other, and is answered as one.
    bool handsLinesOver = true;
    std::function<bool(QTcpSocket*, const QByteArray&)> upgrade;
    // How often somebody began with what is no question to a web server: the opening of an encrypted
    // conversation, for one. Such a caller is hung up on.
    int notHttp = 0;

    int count(const QByteArray& pathBeginning) const
    {
        int found = 0;
        for (const Asked& one : asked) {
            found += one.path.startsWith(pathBeginning) ? 1 : 0;
        }
        return found;
    }

    // The answer that does not end (Answered::streams), on the line it was last begun on.
    // Whether that line is still there:
    bool isStreaming() const { return m_stream != nullptr && m_stream->state() == QAbstractSocket::ConnectedState; }
    // More of it, sent now. False: the line is gone.
    bool sendMore(const QByteArray& bytes)
    {
        if (!isStreaming()) {
            return false;
        }
        m_stream->write(bytes);
        m_stream->flush();
        return true;
    }
    // The server is done with it: what was sent arrives, and the line is closed.
    void endStream()
    {
        if (m_stream != nullptr) {
            m_stream->disconnectFromHost();
        }
    }
    // The server falls over under it: the line is gone without a word.
    void cutStream()
    {
        if (m_stream != nullptr) {
            m_stream->abort();
        }
    }

private:
    void listen(quint16 port)
    {
        m_server->listen(QHostAddress::LocalHost, port);
        // Over TLS a caller is there once the two have agreed on how they speak.
        connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (QTcpSocket* line = m_server->nextPendingConnection()) {
                ++lines;
                m_lineNumbers.insert(line, lines);
                connect(line, &QTcpSocket::readyRead, this, [this, line] { read(line); });
                connect(line, &QTcpSocket::disconnected, this, [this, line] {
                    m_reading.remove(line);
                    m_answeredAt.remove(line);
                    m_lineNumbers.remove(line);
                    line->deleteLater();
                });
                read(line);
            }
        });
    }

    void read(QTcpSocket* line)
    {
        if (forgetsIdleAfterMs > 0 && m_reading.value(line).isEmpty() && m_answeredAt.contains(line) &&
            m_clock.elapsed() - m_answeredAt.value(line) >= forgetsIdleAfterMs && line->bytesAvailable() > 0) {
            ++forgotten;
            line->abort();
            return;
        }
        const QByteArray beginning = line->peek(8192);
        if (upgrade && beginning.contains("\r\n\r\n") && upgrade(line, beginning)) {
            disconnect(line, nullptr, this, nullptr);
            m_reading.remove(line);
            return;
        }
        // Somebody who asks to be made a live line: nothing of the asking is taken from the
        // line here, and the line is the live stand-in's from now on.
        if (const QByteArray head = line->peek(4096).toLower();
            handsLinesOver && m_reading.value(line).isEmpty() && head.contains("\r\n\r\n") &&
            head.contains("upgrade: websocket") && !head.startsWith("get /janus/ws ")) {
            disconnect(line, nullptr, this, nullptr);
            m_reading.remove(line);
            live.take(line);
            return;
        }
        QByteArray& soFar = m_reading[line];
        soFar += line->readAll();
        if (!soFar.isEmpty() && (soFar.front() < 'A' || soFar.front() > 'Z')) {
            ++notHttp;
            m_reading.remove(line);
            line->disconnectFromHost();
            return;
        }
        const qsizetype end = soFar.indexOf("\r\n\r\n");
        if (end < 0) {
            return;
        }
        Asked one;
        const QList<QByteArray> head = soFar.left(end).split('\n');
        const QList<QByteArray> first = head.value(0).trimmed().split(' ');
        one.method = first.value(0);
        one.path = first.value(1);
        for (qsizetype index = 1; index < head.size(); ++index) {
            const qsizetype colon = head[index].indexOf(':');
            if (colon > 0) {
                one.headers.insert(head[index].left(colon).trimmed().toLower(), head[index].mid(colon + 1).trimmed());
            }
        }
        const qsizetype length = one.headers.value("content-length").toLongLong();
        if (soFar.size() < end + 4 + length) {
            return; // the body is still on its way
        }
        one.body = soFar.mid(end + 4, length);
        one.line = m_lineNumbers.value(line);
        m_reading.remove(line);
        asked.append(one);

        if (beforeAnswer) {
            beforeAnswer();
        }
        const Answered answer = m_answer(one);
        if (answer.never) {
            return;
        }
        if (answer.hangUp) {
            line->abort();
            return;
        }
        if (answer.streams) {
            QByteArray begun = "HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\nContent-Type: " + answer.type +
                               "\r\nConnection: close\r\n";
            for (const QByteArray& header : answer.more) {
                begun += header + "\r\n";
            }
            line->write(begun + "\r\n" + answer.body);
            m_stream = line;
            return;
        }
        const bool keeps = keepsLines && !closesSilently;
        QByteArray out =
            "HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\nContent-Type: " + answer.type +
            "\r\nContent-Length: " + QByteArray::number(answer.body.size() + answer.shortBy + answer.moreToCome) +
            (closesSilently ? QByteArray("\r\n")
             : keeps        ? QByteArray("\r\nConnection: keep-alive\r\n")
                            : QByteArray("\r\nConnection: close\r\n"));
        for (const QByteArray& header : answer.more) {
            out += header + "\r\n";
        }
        out += "\r\n" + answer.body;
        // (A caller that has gone meanwhile takes the timer with it.)
        if (answer.byteEveryMs > 0) {
            // What comes before the body at once, and then the body a byte at a time, for as
            // long as the caller stays: never the last of it.
            line->write(out.left(out.size() - answer.body.size()));
            auto* drip = new QTimer(line); // NOLINT(cppcoreguidelines-owning-memory): the line's own
            connect(drip, &QTimer::timeout, line, [line] { line->write("x", 1); });
            drip->start(answer.byteEveryMs);
            return;
        }
        const qsizetype body = answer.body.size();
        QTimer::singleShot(answer.afterMs, line,
                           [this, line, out, body, keeps, breaksOff = answer.shortBy > 0, later = answer.bodyAfterMs,
                            goesOn = answer.moreToCome > 0] {
                               const auto rest = [this, line, keeps, breaksOff, goesOn](const QByteArray& bytes) {
                                   line->write(bytes);
                                   m_answeredAt.insert(line, m_clock.elapsed());
                                   // (A server that is still sending keeps its line, whatever it
                                   // said of keeping lines: whoever asked ends it.)
                                   if ((!keeps && !goesOn) || breaksOff) {
                                       line->disconnectFromHost();
                                   }
                               };
                               if (later <= 0) {
                                   rest(out);
                                   return;
                               }
                               line->write(out.first(out.size() - body));
                               QTimer::singleShot(later, line, [rest, out, body] { rest(out.last(body)); });
                           });
    }

    std::function<Answered(const Asked&)> m_answer;
    bool m_encrypted = false;
    // The server, where it is one over TLS: it is `m_server`'s, and goes with it.
    LateToLetIn* m_late = nullptr;
    std::unique_ptr<QTcpServer> m_server;
    QHash<QTcpSocket*, QByteArray> m_reading;
    QHash<QTcpSocket*, int> m_lineNumbers;
    // When each line was last answered (forgetsIdleAfterMs), by m_clock.
    QHash<QTcpSocket*, qint64> m_answeredAt;
    QElapsedTimer m_clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    QPointer<QTcpSocket> m_stream;
};

// An HTTPS that answers one look as no JetKVM does (a 404), showing the certificate of `tls`, and from then on, at
// that very port, takes every line and says nothing (a TLS conversation that never begins): one that falls silent
// between two looks at it (from the outside review of the desktop, F1). Its address is the same throughout.
class FallsSilent {
public:
    explicit FallsSilent(const Tls& tls)
        : m_stand([](const Asked&) { return Answered(404, R"({"error":"Not Found"})", "application/json"); }, tls),
          m_port(m_stand.serverPort()), m_address(m_stand.address())
    {
        m_stand.beforeAnswer = [this] {
            if (!m_listening) {
                m_stand.takeNoMore();
                m_listening = m_silent.listen(QHostAddress::LocalHost, m_port);
            }
        };
    }

    QString address() const { return m_address; }
    // How often it answered: what was asked on the lines it had taken before it fell silent.
    qsizetype answered() const { return m_stand.asked.size(); }
    // Whether it listens at its port, silent, from its first answer on.
    bool fellSilent() const { return m_listening; }
    // Whether somebody has called it since it fell silent.
    bool calledSilently() const { return m_silent.hasPendingConnections(); }

private:
    StandIn m_stand;
    quint16 m_port;
    QString m_address;
    QTcpServer m_silent;
    bool m_listening = false;
};

} // namespace qkvm::core::testing
