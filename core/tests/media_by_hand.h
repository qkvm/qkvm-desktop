#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <QByteArray>
#include <QCryptographicHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

// A QKVM's route for an image carried to a machine's device, by hand, for the upload tests (DESIGN.md section 8): on
// the loopback address, in a thread of its own, so that what the program's own thread does meanwhile can be watched.
// It reads an image as it comes, at the pace a test names, counting and hashing it and keeping none of it (its line
// takes no more than a quarter of a megabyte ahead: what is not read is the sender's to hold); it says how far it has
// come, as the server does (GET .../media/upload); and it answers as a test says: the image's SHA-256 once all of it
// came, a refusal at once without reading a byte (whose answer the sender may never see: the line is closed under
// what it still sends), a refusal once all of it was read, a line hung up on, or one that stops reading. A look at how
// the machines are is hung up on: a call that finds nothing answering, for a test of what that does to an upload.
namespace qkvm::core::testing {

class UploadByHand {
public:
    struct Way {
        qint64 bytesPerTick = 0; // read at most this much every `tickMs`; 0: as fast as it comes
        int tickMs = 10;
        // Answered at once with this, before a byte of the image is read (a refusal), where `refusal` is not 0.
        int refusal = 0;
        QByteArray refusalBody;
        // The same, once the whole image was read.
        bool refusesAtTheEnd = false;
        // The line hung up on once this much came; and reading stopped once this much came (the line kept).
        qint64 hangUpAfter = -1;
        qint64 stopAfter = -1;
    };

    explicit UploadByHand(Way way) : m_way(std::move(way))
    {
        m_home.moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(
            &m_home,
            [this] {
                m_server = std::make_unique<QTcpServer>();
                m_server->listen(QHostAddress::LocalHost, 0);
                m_port = m_server->serverPort();
                QObject::connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, &m_home, [this] {
                    while (QTcpSocket* line = m_server->nextPendingConnection()) {
                        take(line);
                    }
                });
            },
            Qt::BlockingQueuedConnection);
    }

    ~UploadByHand()
    {
        QMetaObject::invokeMethod(
            &m_home,
            [this] {
                // (Each line that goes is let go of from the list as it goes.)
                QList<QPointer<QTcpSocket>> open;
                for (const auto& [line, at] : m_lines) {
                    open.append(line);
                }
                for (const QPointer<QTcpSocket>& line : open) {
                    if (line != nullptr) {
                        line->abort();
                    }
                }
                m_lines.clear();
                m_server.reset();
            },
            Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }

    UploadByHand(const UploadByHand&) = delete;
    UploadByHand& operator=(const UploadByHand&) = delete;
    UploadByHand(UploadByHand&&) = delete;
    UploadByHand& operator=(UploadByHand&&) = delete;

    QString address() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port.load()); }
    // What came of the image so far, of the last upload; how many uploads were begun; and, once one came whole, its
    // SHA-256.
    qint64 received() const { return m_received; }
    int uploads() const { return m_uploads; }
    QByteArray sha256() const
    {
        const std::scoped_lock held(m_lock);
        return m_sha256;
    }
    // The first line of every question, as it came ("POST /api/v1/...").
    QList<QByteArray> asked() const
    {
        const std::scoped_lock held(m_lock);
        return m_asked;
    }
    // Whether the line the last image came on was closed by whoever sent it before all of it came.
    bool cutBySender() const { return m_cutBySender; }

private:
    struct Line {
        QByteArray head;
        bool inBody = false;
        qint64 length = 0;
        qint64 got = 0;
        QCryptographicHash hash{QCryptographicHash::Sha256};
        QString name;
    };

    void take(QTcpSocket* line)
    {
        // (No more than this much is taken off the line ahead of what is read: the rest waits at the sender's.)
        line->setReadBufferSize(qint64{256} * 1024);
        // (A line is in the list from here until it is gone, and nothing puts it back: what it hears after that,
        // until it is let go of, finds nothing.)
        m_lines.try_emplace(line);
        auto* pace = new QTimer(line); // NOLINT(cppcoreguidelines-owning-memory): the line's own
        pace->setInterval(m_way.tickMs);
        QObject::connect(pace, &QTimer::timeout, line, [this, line] { read(line); });
        QObject::connect(line, &QTcpSocket::readyRead, line, [this, line] {
            const auto at = m_lines.find(line);
            if (at != m_lines.end() && (m_way.bytesPerTick == 0 || !at->second.inBody)) {
                read(line);
            }
        });
        QObject::connect(line, &QTcpSocket::disconnected, line, [this, line, pace] {
            pace->stop();
            if (const auto at = m_lines.find(line); at != m_lines.end()) {
                if (at->second.inBody && at->second.got < at->second.length) {
                    m_cutBySender = true;
                }
                m_lines.erase(at);
            }
            line->deleteLater();
        });
        // (Also as fast as it comes: a line whose buffer was full says nothing more of what waits behind it.)
        pace->start();
    }

    void answer(QTcpSocket* line, int status, const QByteArray& body)
    {
        line->write("HTTP/1.1 " + QByteArray::number(status) +
                    " X\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\n\r\n" + body);
        line->disconnectFromHost();
    }

    void read(QTcpSocket* line)
    {
        const auto found = m_lines.find(line);
        if (found == m_lines.end()) {
            return;
        }
        Line& at = found->second;
        if (!at.inBody) {
            at.head += line->read(8192 - at.head.size());
            const qsizetype end = at.head.indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            const QByteArray rest = at.head.mid(end + 4);
            at.head.truncate(end);
            const QByteArray first = at.head.left(at.head.indexOf("\r\n"));
            {
                const std::scoped_lock held(m_lock);
                m_asked.append(first);
            }
            if (first.startsWith("GET ") && first.contains("/media/upload")) {
                answer(line, 200,
                       m_uploading ? R"({"uploading": {"name": ")" + m_name + R"(", "size": )" +
                                         QByteArray::number(m_length.load()) + R"(, "sent": )" +
                                         QByteArray::number(m_received.load()) + R"(, "by": "anna"}})"
                                   : QByteArray(R"({"uploading": null})"));
                return;
            }
            if (!first.startsWith("POST ") || !first.contains("/media/upload?")) {
                answer(line, 404, R"({"code": "not_found", "message": "Unknown"})");
                return;
            }
            if (first.startsWith("GET /api/v1/status")) {
                line->abort();
                return;
            }
            if (m_way.refusal != 0 && !m_way.refusesAtTheEnd) {
                answer(line, m_way.refusal, m_way.refusalBody);
                return;
            }
            for (const QByteArray& header : at.head.split('\n')) {
                if (header.toLower().startsWith("content-length:")) {
                    at.length = header.mid(15).trimmed().toLongLong();
                }
            }
            at.inBody = true;
            ++m_uploads;
            m_received = 0;
            m_length = at.length;
            m_name = first.mid(first.indexOf("name=") + 5).split('&').value(0);
            m_uploading = true;
            swallow(at, line, rest);
            if (!m_lines.contains(line)) {
                return; // (hung up on: what was its is gone)
            }
        }
        if (m_way.stopAfter >= 0 && at.got >= m_way.stopAfter) {
            return;
        }
        const qint64 most = m_way.bytesPerTick > 0 ? m_way.bytesPerTick : qint64{1} << 40;
        // (No more than is there: QIODevice::read makes room for as much as it is asked for, before it reads.)
        swallow(at, line, line->read(std::min({most, at.length - at.got, line->bytesAvailable()})));
    }

    void swallow(Line& at, QTcpSocket* line, const QByteArray& piece)
    {
        if (piece.isEmpty()) {
            return;
        }
        at.hash.addData(piece);
        at.got += piece.size();
        m_received = at.got;
        if (m_way.hangUpAfter >= 0 && at.got >= m_way.hangUpAfter) {
            m_uploading = false;
            line->abort();
            return;
        }
        if (at.got >= at.length) {
            m_uploading = false;
            const QByteArray sha = at.hash.result().toHex();
            {
                const std::scoped_lock held(m_lock);
                m_sha256 = sha;
            }
            if (m_way.refusesAtTheEnd) {
                answer(line, m_way.refusal, m_way.refusalBody);
                return;
            }
            answer(line, 200, R"({"ok": true, "image": null, "sha256": ")" + sha + "\"}");
        }
    }

    Way m_way;
    QThread m_thread;
    // What lives in the thread: the server and its lines, made there and ended there.
    QObject m_home;
    std::unique_ptr<QTcpServer> m_server;
    std::unordered_map<QTcpSocket*, Line> m_lines;
    std::atomic<quint16> m_port = 0;
    std::atomic<qint64> m_received = 0;
    std::atomic<qint64> m_length = 0;
    std::atomic<int> m_uploads = 0;
    std::atomic<bool> m_uploading = false;
    std::atomic<bool> m_cutBySender = false;
    QByteArray m_name;
    mutable std::mutex m_lock;
    QByteArray m_sha256;
    QList<QByteArray> m_asked;
};

} // namespace qkvm::core::testing
