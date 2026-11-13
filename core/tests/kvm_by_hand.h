#pragma once

#include <memory>
#include <optional>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include "stand_in.h"

// A KVM's end of its line (/api/ws), played by hand: the asking for the line is answered as a test
// says, and from then on every byte that comes is kept as it came, to be taken apart by the test,
// and what goes back is put together by the test. For what a server that speaks WebSocket properly
// hides or cannot be made to do: whether a message came hidden as a caller's has to be, an answer
// that is none, a KVM that takes nothing off the line. On the loopback address, at a port Windows
// chooses; in the clear, or over TLS with a certificate a test names.
namespace qkvm::core::testing {

class KvmByHand : public QObject {
public:
    explicit KvmByHand(const std::optional<Tls>& tls = {}) : m_encrypted(tls.has_value())
    {
        if (tls) {
            auto server = std::make_unique<QSslServer>();
            QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
            configuration.setLocalCertificateChain(QSslCertificate::fromPath(tls->certificateFile, QSsl::Pem));
            QFile key(tls->keyFile);
            if (key.open(QIODevice::ReadOnly)) {
                configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
            }
            server->setSslConfiguration(configuration);
            m_server = std::move(server);
        } else {
            m_server = std::make_unique<QTcpServer>();
        }
        m_server->listen(QHostAddress::LocalHost);
        // Over TLS a caller is there once the two have agreed on how they speak.
        connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (QTcpSocket* line = m_server->nextPendingConnection()) {
                ++lines;
                m_line = line;
                m_asked = false;
                m_came.clear();
                connect(line, &QTcpSocket::readyRead, this, [this, line] { hear(line); });
                connect(line, &QTcpSocket::disconnected, this, [this, line] {
                    if (m_line == line) {
                        if (line->isOpen()) {
                            hear(line); // what came with the end of the line
                        }
                        m_line = nullptr;
                    }
                    line->deleteLater();
                });
                hear(line);
            }
        });
    }

    // The server goes first, and its callers with it, while what they are noted in is still there.
    ~KvmByHand() override { m_server.reset(); }
    KvmByHand(const KvmByHand&) = delete;
    KvmByHand& operator=(const KvmByHand&) = delete;
    KvmByHand(KvmByHand&&) = delete;
    KvmByHand& operator=(KvmByHand&&) = delete;

    QUrl address() const
    {
        return QUrl(QStringLiteral("%1://127.0.0.1:%2")
                        .arg(m_encrypted ? QStringLiteral("https") : QStringLiteral("http"))
                        .arg(m_server->serverPort()));
    }

    // What it does with an asking for the line: lets the caller in, as a KVM does; or says this
    // instead, whole as it is written here; or says nothing until the test has it let the caller in.
    QByteArray instead;
    bool answers = true;
    void letIn()
    {
        if (m_line == nullptr || !m_asked) {
            return;
        }
        const QByteArray mark =
            QCryptographicHash::hash(headers.value("sec-websocket-key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                     QCryptographicHash::Sha1)
                .toBase64();
        m_line->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " +
                      mark + "\r\n\r\n");
    }

    // How many callers there were, and how many bytes came from all of them, the askings included.
    // Of the last caller: whether it is still on its line, where it asked for the line and what
    // the asking carried.
    int lines = 0;
    qint64 bytesHeard = 0;
    bool isOn() const { return m_line != nullptr; }
    bool wasAsked() const { return m_asked; }
    QByteArray path;
    QHash<QByteArray, QByteArray> headers; // names in small letters
    // What came from the last caller after its asking, as it stood on the line.
    QByteArray afterTheAsking() const { return m_asked ? m_came : QByteArray(); }
    qsizetype bytesAfterTheAsking() const { return afterTheAsking().size(); }

    // One message as it came on the line.
    struct Message {
        int kind = 0;        // 1 text, 8 goodbye, 9 "are you there", 10 the answer to that
        bool whole = false;  // not a part of one that goes on
        bool spare = false;  // one of the three marks nobody has agreed on is set
        bool hidden = false; // as a caller has to hide what it sends
        int lengthBytes = 0; // how its length was written: in 1, 3 or 9 bytes
        QByteArray hiddenBy; // the four bytes it was hidden with
        QByteArray what;     // what it says, read as it was meant
        QByteArray asItCame; // and as it stood on the line
    };
    // Every message of the last caller's that has come whole, in the order they came.
    QList<Message> messages() const
    {
        QList<Message> found;
        qsizetype at = 0;
        while (m_came.size() - at >= 2) {
            const auto first = static_cast<quint8>(m_came.at(at));
            const auto second = static_cast<quint8>(m_came.at(at + 1));
            const int said = second & 0x7F;
            const qsizetype beginning = said == 126 ? 4 : said == 127 ? 10 : 2;
            if (m_came.size() - at < beginning) {
                break;
            }
            qsizetype length = said;
            if (said >= 126) {
                length = 0;
                for (qsizetype index = 2; index < beginning; ++index) {
                    length = (length << 8) | static_cast<quint8>(m_came.at(at + index));
                }
            }
            Message one;
            one.kind = first & 0x0F;
            one.whole = (first & 0x80) != 0;
            one.spare = (first & 0x70) != 0;
            one.hidden = (second & 0x80) != 0;
            one.lengthBytes = static_cast<int>(beginning) - 1;
            const qsizetype before = beginning + (one.hidden ? 4 : 0);
            if (length < 0 || m_came.size() - at - before < length) {
                break;
            }
            one.asItCame = m_came.mid(at + before, length);
            one.what = one.asItCame;
            if (one.hidden) {
                one.hiddenBy = m_came.mid(at + beginning, 4);
                for (qsizetype index = 0; index < length; ++index) {
                    one.what[index] = static_cast<char>(one.what.at(index) ^ one.hiddenBy.at(index % 4));
                }
            }
            found.append(one);
            at += before + length;
        }
        return found;
    }
    // The texts among them, read as the JSON they are.
    QList<QJsonObject> said() const
    {
        QList<QJsonObject> found;
        const QList<Message> all = messages();
        for (const Message& one : all) {
            if (one.kind == 1) {
                found.append(QJsonDocument::fromJson(one.what).object());
            }
        }
        return found;
    }
    // The messages of one kind.
    QList<Message> messagesOf(int kind) const
    {
        QList<Message> found;
        const QList<Message> all = messages();
        for (const Message& one : all) {
            if (one.kind == kind) {
                found.append(one);
            }
        }
        return found;
    }

    // One message as a KVM sends it: whole and not hidden.
    void send(int kind, const QByteArray& what = {})
    {
        QByteArray message;
        message += static_cast<char>(0x80 | kind);
        if (what.size() <= 125) {
            message += static_cast<char>(what.size());
        } else if (what.size() <= 0xFFFF) {
            message += static_cast<char>(126);
            message += static_cast<char>((what.size() >> 8) & 0xFF);
            message += static_cast<char>(what.size() & 0xFF);
        } else {
            message += static_cast<char>(127);
            for (int shift = 56; shift >= 0; shift -= 8) {
                message += static_cast<char>((static_cast<quint64>(what.size()) >> shift) & 0xFF);
            }
        }
        sendRaw(message + what);
    }
    void sendRaw(const QByteArray& bytes)
    {
        if (m_line != nullptr) {
            m_line->write(bytes);
        }
    }
    // Hands what was sent to Windows now, and not at the next turn of the loop: for a test in which
    // something of the KVM's is to be on the line before the program next looks.
    void pushOut()
    {
        if (m_line != nullptr) {
            m_line->flush();
        }
    }
    // Ends the line in order, as a KVM does that was said goodbye to: what it sent arrives first.
    void endLine()
    {
        if (m_line != nullptr) {
            m_line->disconnectFromHost();
        }
    }
    // From now on nothing that comes is taken off the line, and Windows keeps little of it.
    void stopReading()
    {
        m_reads = false;
        if (m_line != nullptr) {
            m_line->setReadBufferSize(1024);
            m_line->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        }
    }
    // And takes it off the line again, what waited meanwhile first: a way that had stalled, and
    // comes back.
    void readAgain()
    {
        m_reads = true;
        if (m_line != nullptr) {
            m_line->setReadBufferSize(0);
            hear(m_line);
        }
    }
    // Falls over: the line is gone without a word.
    void fallOver()
    {
        if (m_line != nullptr) {
            m_line->abort();
        }
    }
    // The last caller's line as Windows knows it, for a test that asks Windows itself what has come
    // on it and how it ended: of a line that was reset, Qt says what it says of one that was closed.
    // Only of a KVM that has stopped reading and holds as much as it keeps: until then Qt reads the
    // line itself. -1: there is none.
    qintptr descriptor() const { return m_line != nullptr ? m_line->socketDescriptor() : -1; }

private:
    void hear(QTcpSocket* line)
    {
        if (!m_reads || line != m_line) {
            return;
        }
        const QByteArray came = line->readAll();
        bytesHeard += came.size();
        m_came += came;
        if (m_asked) {
            return;
        }
        const qsizetype stop = m_came.indexOf("\r\n\r\n");
        if (stop < 0) {
            return;
        }
        const QList<QByteArray> head = m_came.left(stop).split('\n');
        m_came.remove(0, stop + 4);
        path = head.value(0).trimmed().split(' ').value(1);
        headers.clear();
        for (qsizetype index = 1; index < head.size(); ++index) {
            const qsizetype colon = head[index].indexOf(':');
            if (colon > 0) {
                headers.insert(head[index].left(colon).trimmed().toLower(), head[index].mid(colon + 1).trimmed());
            }
        }
        m_asked = true;
        if (!instead.isEmpty()) {
            line->write(instead);
        } else if (answers) {
            letIn();
        }
    }

    bool m_encrypted = false;
    std::unique_ptr<QTcpServer> m_server;
    QTcpSocket* m_line = nullptr;
    QByteArray m_came; // of the last caller: its asking until that is whole, then what came after it
    bool m_asked = false;
    bool m_reads = true;
};

} // namespace qkvm::core::testing
