#pragma once

#include <memory>
#include <utility>

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtEndian>

// The other end of a live line, for the tests of the program's own: it takes whoever comes, notes
// what was asked for and every word that is said to it, and says and sends what a test tells it to
// (DESIGN.md section 8). On the loopback address, at a port Windows chooses; in the clear, or
// over TLS with a certificate a test names.
namespace qkvm::core::testing {

class LiveStandIn : public QObject {
public:
    LiveStandIn()
        : m_server(std::make_unique<QWebSocketServer>(QStringLiteral("stand-in"), QWebSocketServer::NonSecureMode))
    {
        listen();
    }

    // One that listens nowhere by itself: callers are handed to it (`take`) by a web server that
    // found them asking for a live line, as a QKVM has both at one address.
    enum class Handed { Over };
    explicit LiveStandIn(Handed /*over*/)
        : m_server(std::make_unique<QWebSocketServer>(QStringLiteral("stand-in"), QWebSocketServer::NonSecureMode))
    {
        watch();
    }
    void take(QTcpSocket* caller)
    {
        m_taken = caller;
        m_server->handleConnection(caller);
        // What the caller asked is on the line already, and was said to be there before the
        // line was handed over: said once more, for the one who listens now.
        emit caller->readyRead();
    }

    // Over TLS, with the certificate and the key in these two files.
    LiveStandIn(const QString& certificateFile, const QString& keyFile)
        : m_encrypted(true),
          m_server(std::make_unique<QWebSocketServer>(QStringLiteral("stand-in"), QWebSocketServer::SecureMode))
    {
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        // The server's own certificate, and after it whichever a server sends along with it.
        QFile certificate(certificateFile);
        if (certificate.open(QIODevice::ReadOnly)) {
            configuration.setLocalCertificateChain(QSslCertificate::fromData(certificate.readAll(), QSsl::Pem));
        }
        QFile key(keyFile);
        if (key.open(QIODevice::ReadOnly)) {
            configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
        }
        m_server->setSslConfiguration(configuration);
        listen();
    }

    // The server goes first, and its callers with it, while what they are noted in is still there.
    ~LiveStandIn() override { m_server.reset(); }
    LiveStandIn(const LiveStandIn&) = delete;
    LiveStandIn& operator=(const LiveStandIn&) = delete;
    LiveStandIn(LiveStandIn&&) = delete;
    LiveStandIn& operator=(LiveStandIn&&) = delete;

    quint16 serverPort() const { return m_server->serverPort(); }
    // As a QKVM is named: the live line is at the same place as its other calls.
    QString address() const
    {
        return QStringLiteral("%1://127.0.0.1:%2")
            .arg(m_encrypted ? QStringLiteral("https") : QStringLiteral("http"))
            .arg(serverPort());
    }

    // How many lines were taken; of the last one, where it was asked for and what it carried; and
    // every word that was said on any of them, in the order they came.
    int lines = 0;
    QByteArray path;
    QHash<QByteArray, QByteArray> headers; // names in small letters
    QList<QJsonObject> said;
    // What it says to each line a moment after it is taken, as a kvmd KVM says its states on a line to its events.
    QStringList greeting;
    // Every path a line was asked for at, in the order they came.
    QList<QByteArray> paths;
    bool isConnected() const { return m_line != nullptr; }

    // The words of one kind that were said, in the order they came.
    QList<QJsonObject> saidOf(const QString& kind) const
    {
        QList<QJsonObject> found;
        for (const QJsonObject& word : said) {
            if (word[QStringLiteral("t")].toString() == kind) {
                found.append(word);
            }
        }
        return found;
    }

    // What the server says, and the pictures it sends, to whoever is on the line.
    void say(const QJsonObject& word)
    {
        if (m_line != nullptr) {
            m_line->sendTextMessage(QString::fromUtf8(QJsonDocument(word).toJson(QJsonDocument::Compact)));
        }
    }
    void sayRaw(const QString& text)
    {
        if (m_line != nullptr) {
            m_line->sendTextMessage(text);
        }
    }
    void sendRaw(const QByteArray& message)
    {
        if (m_line != nullptr) {
            m_line->sendBinaryMessage(message);
        }
    }
    // One picture as the draft has it: kind 1 is H.264, 2 a JPEG.
    void send(char kind, bool keyFrame, quint32 counter, qint64 takenUs, const QByteArray& data)
    {
        QByteArray message(16, '\0');
        message[0] = kind;
        message[1] = keyFrame ? 1 : 0;
        qToLittleEndian<quint32>(counter, message.data() + 4);
        qToLittleEndian<qint64>(takenUs, message.data() + 8);
        sendRaw(message + data);
    }
    // Closes the line, with a last word where a code is given.
    void hangUp(const QString& code = {}, const QString& message = {})
    {
        if (m_line == nullptr) {
            return;
        }
        if (!code.isEmpty()) {
            say({{QStringLiteral("t"), QStringLiteral("bye")},
                 {QStringLiteral("code"), code},
                 {QStringLiteral("message"), message}});
        }
        m_line->close();
    }
    // Falls over: the line is gone without a word.
    void fallOver()
    {
        if (m_line != nullptr) {
            m_line->abort();
        }
    }
    // The way to whoever is on the line dies, as far as that one can tell: from now on nothing
    // that comes on it is read here, nothing is answered, a goodbye least of all, and the line
    // is never ended from here. What is sent to it, Windows takes all the same. (Of a line that
    // was handed over, the last one: nothing that listens to it hears it any more.)
    void hearNoMore()
    {
        if (m_taken != nullptr) {
            const QTcpSocket* line = m_taken.data();
            disconnect(line, &QTcpSocket::readyRead, nullptr, nullptr);
            disconnect(line, &QTcpSocket::readChannelFinished, nullptr, nullptr);
            disconnect(line, &QTcpSocket::disconnected, nullptr, nullptr);
            disconnect(line, &QTcpSocket::errorOccurred, nullptr, nullptr);
            disconnect(line, &QTcpSocket::stateChanged, nullptr, nullptr);
        }
    }

private:
    // kvmd takes keys and the mouse in short messages of bytes as well as in texts (kvmd's own
    // page speaks the bytes): what such a message says is noted as the text that says the same,
    // so that a test reads one list whichever way it was said. What kvmd would not read is noted
    // as that.
    static QJsonObject readAsKvmdReadsBytes(const QByteArray& message)
    {
        const auto both = [](int x, int y) { return QJsonObject{{QStringLiteral("x"), x}, {QStringLiteral("y"), y}}; };
        const auto word = [](const QString& kind, const QJsonObject& event) {
            return QJsonObject{{QStringLiteral("event_type"), kind}, {QStringLiteral("event"), event}};
        };
        const auto small = [&message](qsizetype at) { return static_cast<int>(static_cast<qint8>(message.at(at))); };
        const int kind = message.isEmpty() ? 0 : message.at(0);
        if ((kind == 1 || kind == 2) && message.size() > 2 && (message.at(1) == 0 || message.at(1) == 1)) {
            const QString name = QString::fromLatin1(message.sliced(2));
            const bool down = message.at(1) == 1;
            return kind == 1
                       ? word(QStringLiteral("key"), {{QStringLiteral("key"), name}, {QStringLiteral("state"), down}})
                       : word(QStringLiteral("mouse_button"),
                              {{QStringLiteral("button"), name}, {QStringLiteral("state"), down}});
        }
        if (kind == 3 && message.size() == 5) {
            return word(QStringLiteral("mouse_move"),
                        {{QStringLiteral("to"), both(qFromBigEndian<qint16>(message.constData() + 1),
                                                     qFromBigEndian<qint16>(message.constData() + 3))}});
        }
        if ((kind == 4 || kind == 5) && message.size() == 4 && message.at(1) == 0) {
            return word(kind == 4 ? QStringLiteral("mouse_relative") : QStringLiteral("mouse_wheel"),
                        {{QStringLiteral("delta"), both(small(2), small(3))}});
        }
        return word(QStringLiteral("bytes kvmd would not read"),
                    {{QStringLiteral("hex"), QString::fromLatin1(message.toHex())}});
    }

    void listen()
    {
        m_server->listen(QHostAddress::LocalHost);
        watch();
    }

    void watch()
    {
        connect(m_server.get(), &QWebSocketServer::newConnection, this, [this] {
            while (QWebSocket* line = m_server->nextPendingConnection()) {
                ++lines;
                m_line = line;
                path = line->requestUrl().toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority);
                paths.append(path);
                for (const QString& word : std::as_const(greeting)) {
                    QTimer::singleShot(20, line, [line, word] { line->sendTextMessage(word); });
                }
                headers.clear();
                const QNetworkRequest asked = line->request();
                const QList<QByteArray> names = asked.rawHeaderList();
                for (const QByteArray& name : names) {
                    headers.insert(name.toLower(), asked.rawHeader(name));
                }
                connect(line, &QWebSocket::textMessageReceived, this,
                        [this](const QString& text) { said.append(QJsonDocument::fromJson(text.toUtf8()).object()); });
                connect(line, &QWebSocket::binaryMessageReceived, this,
                        [this](const QByteArray& message) { said.append(readAsKvmdReadsBytes(message)); });
                connect(line, &QWebSocket::disconnected, this, [this, line] {
                    if (m_line == line) {
                        m_line = nullptr;
                    }
                    line->deleteLater();
                });
            }
        });
    }

    bool m_encrypted = false;
    std::unique_ptr<QWebSocketServer> m_server;
    QWebSocket* m_line = nullptr;
    QPointer<QTcpSocket> m_taken; // the line that was handed over last, as it came
};

} // namespace qkvm::core::testing
