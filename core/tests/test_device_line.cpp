#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUrl>

#include <qkvm/core/device_line.h>
#include <qkvm/core/tls.h>
#include <qkvm/core/video_diagnostics.h>

#include "line_threads.h"

#include <winsock2.h>

using namespace Qt::StringLiterals;
using qkvm::core::DeviceLine;

namespace {

// The tests' certificate "first", which a device's line over TLS is known by here.
QString firstCertificate()
{
    return QStringLiteral(QKVM_TESTDATA "/tls/first.crt");
}
QString firstKey()
{
    return QStringLiteral(QKVM_TESTDATA "/tls/first.key");
}
QByteArray firstsSha256()
{
    const QList<QSslCertificate> chain = QSslCertificate::fromPath(firstCertificate(), QSsl::Pem);
    return chain.isEmpty() ? QByteArray() : chain.first().digest(QCryptographicHash::Sha256);
}

// A device's line made by hand, for what one that speaks WebSocket properly cannot be made to do: it
// takes the asking, answers it with "you are in" and whatever a test puts beside that, and sends what a
// test gives it as WebSocket messages. In the clear, or over TLS (with the certificate "first") where asked.
class HandMadeLine : public QObject {
public:
    explicit HandMadeLine(bool overTls = false) : m_overTls(overTls)
    {
        if (overTls) {
            // (Windows' own TLS, as the program's: chosen before the server is made, which would take Qt's own pick.)
            static_cast<void>(qkvm::core::useWindowsOwnTls());
            auto server = std::make_unique<QSslServer>();
            QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
            configuration.setLocalCertificateChain(QSslCertificate::fromPath(firstCertificate(), QSsl::Pem));
            QFile key(firstKey());
            if (key.open(QIODevice::ReadOnly)) {
                configuration.setPrivateKey(QSslKey(key.readAll(), QSsl::Rsa, QSsl::Pem));
            }
            server->setSslConfiguration(configuration);
            m_server = std::move(server);
        } else {
            m_server = std::make_unique<QTcpServer>();
        }
        m_server->listen(QHostAddress::LocalHost);
        const auto take = [this] {
            while (QTcpSocket* line = m_server->nextPendingConnection()) {
                m_line = line;
                connect(line, &QTcpSocket::readyRead, this, [this, line] { hear(line); });
                connect(line, &QTcpSocket::disconnected, this, [this, line] {
                    hear(line);
                    m_ended = true;
                });
                if (auto* const tls = qobject_cast<QSslSocket*>(line)) {
                    connect(tls, &QSslSocket::encryptedBytesWritten, this,
                            [this](qint64 written) { m_hiddenWritten += written; });
                }
            }
        };
        // (Over TLS a line is pending once its handshake is over.)
        if (overTls) {
            connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, this, take);
        } else {
            connect(m_server.get(), &QTcpServer::newConnection, this, take);
        }
    }

    // What stands beside "you are in" and its mark.
    QByteArray besides = "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    // What it answers the asking with instead of "you are in", where set.
    QByteArray instead;
    // The asking, as it came.
    QByteArray asking;

    QUrl address() const
    {
        return QUrl(u"%1://127.0.0.1:%2"_s.arg(m_overTls ? u"https"_s : u"http"_s).arg(m_server->serverPort()));
    }

    // One whole message, of text (0x81) or of bytes (0x82), of up to 65535 bytes.
    void send(char kind, const QByteArray& message)
    {
        QByteArray frame;
        frame.append(kind);
        if (message.size() < 126) {
            frame.append(static_cast<char>(message.size()));
        } else {
            frame.append(static_cast<char>(126));
            frame.append(static_cast<char>((message.size() >> 8) & 0xFF));
            frame.append(static_cast<char>(message.size() & 0xFF));
        }
        if (m_line != nullptr) {
            m_line->write(frame + message);
        }
    }

    // What it sent that has not left for the line yet (Windows took the rest). (Over TLS, what is sent is hidden at
    // once, and waits as that.)
    qint64 waiting() const
    {
        if (m_line == nullptr) {
            return 0;
        }
        const auto* const tls = qobject_cast<const QSslSocket*>(m_line.data());
        return m_line->bytesToWrite() + (tls != nullptr ? tls->encryptedBytesToWrite() : 0);
    }

    // Over TLS, all it has made of what it sent, as that is on the line (hidden, in records): what Windows took of it,
    // and what waits for Windows in its socket. So a case can tell how long a record it made is.
    qint64 hiddenSoFar() const
    {
        const auto* const tls = qobject_cast<const QSslSocket*>(m_line.data());
        return tls == nullptr ? 0 : m_hiddenWritten + tls->encryptedBytesToWrite();
    }

    // Bytes as they are, whatever they say of themselves.
    void sendRaw(const QByteArray& bytes)
    {
        if (m_line != nullptr) {
            m_line->write(bytes);
        }
    }

    // Hands what was sent to Windows now, and not at the next turn of the loop: for a test in which what it sent is
    // to be on the line before the program next looks.
    void pushOut()
    {
        if (m_line != nullptr) {
            m_line->flush();
        }
    }
    // What came after the asking, as it came (hidden as a caller hides it); and whether the line has ended.
    QByteArray afterAsking() const
    {
        const qsizetype end = asking.indexOf("\r\n\r\n");
        return end < 0 ? QByteArray() : asking.mid(end + 4);
    }
    bool ended() const { return m_ended; }

    // Takes nothing more off the line (what Windows keeps for it fills, and then what is said waits in the line's
    // program), until it reads again.
    void stopReading()
    {
        m_reads = false;
        if (m_line != nullptr) {
            m_line->setReadBufferSize(1024);
            m_line->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        }
    }
    void readAgain()
    {
        m_reads = true;
        if (m_line != nullptr) {
            m_line->setReadBufferSize(0);
            m_line->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
            hear(m_line);
        }
    }

private:
    void hear(QTcpSocket* line)
    {
        if (!m_reads) {
            return;
        }
        asking += line->readAll();
        if (m_answered || !asking.contains("\r\n\r\n")) {
            return;
        }
        m_answered = true;
        if (!instead.isEmpty()) {
            line->write(instead);
            return;
        }
        const qsizetype at = asking.toLower().indexOf("sec-websocket-key:");
        const QByteArray key = asking.mid(at + 18, asking.indexOf("\r\n", at) - at - 18).trimmed();
        const QByteArray mark =
            QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
        line->write("HTTP/1.1 101 Switching Protocols\r\n" + besides + "Sec-WebSocket-Accept: " + mark + "\r\n\r\n");
    }

    bool m_overTls = false;
    std::unique_ptr<QTcpServer> m_server;
    QPointer<QTcpSocket> m_line;
    qint64 m_hiddenWritten = 0;
    bool m_answered = false;
    bool m_reads = true;
    bool m_ended = false;
};

// The far end of a raw line (a VNC server's TCP connection): it keeps every byte it is sent, sends what a test gives
// it, and notes the end of the line.
class RawPeer : public QObject {
public:
    RawPeer()
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* line = m_server.nextPendingConnection()) {
                m_line = line;
                connect(line, &QTcpSocket::readyRead, this, [this, line] { heard += line->readAll(); });
                connect(line, &QTcpSocket::disconnected, this, [this, line] {
                    heard += line->readAll();
                    ended = true;
                });
            }
        });
    }

    QUrl address() const { return QUrl(u"http://127.0.0.1:%1"_s.arg(m_server.serverPort())); }
    void say(const QByteArray& bytes)
    {
        if (m_line != nullptr) {
            m_line->write(bytes);
        }
    }

    QByteArray heard;
    bool ended = false;

private:
    QTcpServer m_server;
    QPointer<QTcpSocket> m_line;
};

// When, on a clock both threads read.
qint64 nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A device's far end on a thread of its own, by Windows' own calls and nothing of Qt's, for a test that has to see
// what arrives, whatever this program's threads are doing: it lets the line in (WebSocket's "you are in" to whatever
// asking comes), takes nothing off it until it is told to read, and from then on takes everything, and counts it. Its
// thread waits in Windows' recv. What arrived just before a reset and was not read yet Windows may throw away here, and
// when it came is known only once this thread has run: what went to Windows, and when, a case sees where the line's
// thread hands it on (HandedToWindows), and holds what arrived here to that by count alone. Windows is given little
// room for the line on this side (four kilobytes), so that what is said soon waits in the line's program. `raw`: no
// WebSocket (a VNC server's TCP connection): nothing is waited for or answered before it reads.
class FarEndOnItsOwnThread {
public:
    explicit FarEndOnItsOwnThread(bool raw = false) : m_raw(raw)
    {
        WSADATA winsock{};
        ::WSAStartup(MAKEWORD(2, 2), &winsock);
        m_listening = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in here{};
        here.sin_family = AF_INET;
        here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = static_cast<int>(sizeof(here));
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Winsock takes every address as a sockaddr
        const bool listening =
            ::bind(m_listening, reinterpret_cast<const sockaddr*>(&here), static_cast<int>(sizeof(here))) == 0 &&
            ::listen(m_listening, 1) == 0 && ::getsockname(m_listening, reinterpret_cast<sockaddr*>(&here), &size) == 0;
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        if (!listening) {
            return; // (no port: the line to it is not reached, and the case says so)
        }
        m_port = ntohs(here.sin_port);
        m_thread = std::thread([this] { run(); });
    }
    // (Its sockets closed under it end whatever its thread waits for.)
    ~FarEndOnItsOwnThread()
    {
        m_reading = true;
        ::closesocket(m_listening);
        if (const SOCKET line = m_line.load(); line != INVALID_SOCKET) {
            ::closesocket(line);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        ::WSACleanup();
    }
    FarEndOnItsOwnThread(const FarEndOnItsOwnThread&) = delete;
    FarEndOnItsOwnThread& operator=(const FarEndOnItsOwnThread&) = delete;
    FarEndOnItsOwnThread(FarEndOnItsOwnThread&&) = delete;
    FarEndOnItsOwnThread& operator=(FarEndOnItsOwnThread&&) = delete;

    QUrl address() const { return QUrl(u"http://127.0.0.1:%1"_s.arg(m_port)); }
    void startReading() { m_reading = true; }
    // Whether the line came (Windows' accept returned).
    bool accepted() const { return m_accepted.load(); }
    // Sends these on the line, from the calling thread (Windows takes both ways at once).
    void send(QByteArrayView bytes)
    {
        if (const SOCKET line = m_line.load(); line != INVALID_SOCKET) {
            ::send(line, bytes.data(), static_cast<int>(bytes.size()), 0);
        }
    }
    // Ends its side of the line, as a device that closes it does: the line hears its end, and this hears on.
    void endItsSide()
    {
        if (const SOCKET line = m_line.load(); line != INVALID_SOCKET) {
            ::shutdown(line, SD_SEND);
        }
    }
    // All that came after the asking (and its answer), and of the asking (a WebSocket's) what came; and how the line
    // ended ("" while it stands, "closed", "reset", or Windows' number).
    qint64 heard() const { return m_heard.load(); }
    qint64 askingCame() const { return m_askingCame.load(); }
    QByteArray end() const
    {
        const std::scoped_lock locked(m_mutex);
        return m_end;
    }

private:
    void run()
    {
        const SOCKET line = ::accept(m_listening, nullptr, nullptr);
        if (line == INVALID_SOCKET) {
            return;
        }
        m_line = line;
        const int room = 4096;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Winsock takes every option as bytes
        ::setsockopt(line, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&room), static_cast<int>(sizeof(room)));
        m_accepted = true;
        std::vector<char> some(std::size_t{1024} * 1024);
        if (!m_raw) {
            QByteArray asking;
            while (!asking.contains("\r\n\r\n")) {
                const int came = ::recv(line, some.data(), 4096, 0);
                if (came <= 0) {
                    endedSo(came);
                    return;
                }
                m_askingCame += came;
                asking.append(some.data(), came);
            }
            const qsizetype at = asking.toLower().indexOf("sec-websocket-key:");
            const QByteArray key = asking.mid(at + 18, asking.indexOf("\r\n", at) - at - 18).trimmed();
            const QByteArray answer =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                "Upgrade\r\nSec-WebSocket-Accept: " +
                QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1)
                    .toBase64() +
                "\r\n\r\n";
            ::send(line, answer.constData(), static_cast<int>(answer.size()), 0);
        }
        while (!m_reading) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (true) {
            const int came = ::recv(line, some.data(), static_cast<int>(some.size()), 0);
            if (came > 0) {
                m_heard += came;
                continue;
            }
            endedSo(came);
            return;
        }
    }
    // (What recv's end of the line said: none, or an error.)
    void endedSo(int came)
    {
        const int why = ::WSAGetLastError();
        const std::scoped_lock locked(m_mutex);
        m_end = came == 0 ? "closed"_ba : why == WSAECONNRESET ? "reset"_ba : QByteArray::number(why);
    }

    bool m_raw = false;
    SOCKET m_listening = INVALID_SOCKET;
    std::atomic<SOCKET> m_line{INVALID_SOCKET};
    quint16 m_port = 0;
    std::atomic<bool> m_reading{false};
    std::atomic<bool> m_accepted{false};
    std::atomic<qint64> m_heard{0};
    std::atomic<qint64> m_askingCame{0};
    mutable std::mutex m_mutex;
    QByteArray m_end;
    std::thread m_thread;
};

// A HandMadeLine on a thread of its own: a device that goes on sending while this thread is busy.
class HandMadeLineElsewhere {
public:
    explicit HandMadeLineElsewhere(bool overTls)
    {
        m_there->moveToThread(&m_thread);
        m_thread.start();
        QMetaObject::invokeMethod(
            m_there.get(),
            [this, overTls] {
                m_device = std::make_unique<HandMadeLine>(overTls);
                m_address = m_device->address();
            },
            Qt::BlockingQueuedConnection);
    }
    ~HandMadeLineElsewhere()
    {
        QMetaObject::invokeMethod(m_there.get(), [this] { m_device.reset(); }, Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
    HandMadeLineElsewhere(const HandMadeLineElsewhere&) = delete;
    HandMadeLineElsewhere& operator=(const HandMadeLineElsewhere&) = delete;
    HandMadeLineElsewhere(HandMadeLineElsewhere&&) = delete;
    HandMadeLineElsewhere& operator=(HandMadeLineElsewhere&&) = delete;

    QUrl address() const { return m_address; }
    // So many messages of bytes, of so many bytes each (65535 at the most), all handed to its socket at once.
    void sendMany(int messages, qsizetype bytes)
    {
        QMetaObject::invokeMethod(
            m_there.get(),
            [this, messages, bytes] {
                const QByteArray one(bytes, 'f');
                for (int sent = 0; sent < messages; ++sent) {
                    m_device->send('\x82', one);
                }
                m_device->pushOut();
            },
            Qt::BlockingQueuedConnection);
    }
    // What it sent that has not left for the line yet (HandMadeLine::waiting).
    qint64 waiting()
    {
        qint64 waits = 0;
        QMetaObject::invokeMethod(
            m_there.get(), [this, &waits] { waits = m_device->waiting(); }, Qt::BlockingQueuedConnection);
        return waits;
    }

private:
    QThread m_thread;
    std::unique_ptr<QObject> m_there = std::make_unique<QObject>();
    std::unique_ptr<HandMadeLine> m_device;
    QUrl m_address;
};

// When a line's thread was held, on the clock of nowNs (both 0 until it was). What it is told is its own: a thread
// held there could go on after the case that held it.
struct Held {
    std::atomic<qint64> fromNs{0};
    std::atomic<qint64> untilNs{0};
};

// Each line's thread that runs now held there for `ms`, at its next turn, as Windows holds it while it judges a
// certificate (lines::askEach): once all `threads` of them are held, when; none where not so many were, or not in 2 s.
std::vector<std::shared_ptr<Held>> holdLinesThreads(int ms, int threads)
{
    std::vector<std::shared_ptr<Held>> all;
    all.reserve(static_cast<std::size_t>(threads));
    for (int one = 0; one < threads; ++one) {
        all.push_back(std::make_shared<Held>());
    }
    auto next = std::make_shared<std::atomic<int>>(0);
    const int asked = qkvm::core::lines::askEach([all, next, ms] {
        const auto at = static_cast<std::size_t>(next->fetch_add(1));
        const std::shared_ptr<Held> it = at < all.size() ? all.at(at) : std::make_shared<Held>();
        it->fromNs = nowNs();
        QThread::msleep(static_cast<unsigned long>(ms));
        it->untilNs = nowNs();
    });
    if (asked != threads) {
        return {};
    }
    for (int waited = 0; waited < 2000; ++waited) {
        if (std::ranges::all_of(all, [](const std::shared_ptr<Held>& one) { return one->fromNs.load() != 0; })) {
            return all;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

// What a line's thread said it had handed Windows of all that was said on its line, and when, on the clock of nowNs
// (lines::whatWasHanded), from its making to its end: for a case that has to see what went to Windows, and when, on the
// thread that handed it on. The far end cannot: what came just before a reset Windows may throw away there unread, and
// its times are its own thread's, taken after it read (outside review 2, 2.3). Taken on that thread as Held is, so that
// what was handed by the end of a hold there is known to the instant. For one line at a time, in the clear, ended from
// this program alone (as the seam says).
class HandedToWindows {
public:
    HandedToWindows()
    {
        qkvm::core::lines::whatWasHanded([said = m_said](qint64 bytes) {
            const qint64 atNs = nowNs();
            const std::scoped_lock locked(said->mutex);
            said->all.emplace_back(bytes, atNs);
        });
    }
    ~HandedToWindows() { qkvm::core::lines::whatWasHanded({}); }
    HandedToWindows(const HandedToWindows&) = delete;
    HandedToWindows& operator=(const HandedToWindows&) = delete;
    HandedToWindows(HandedToWindows&&) = delete;
    HandedToWindows& operator=(HandedToWindows&&) = delete;

    // All that was handed to Windows before `atNs` (as that thread said it last before then); and all, by now.
    qint64 before(qint64 atNs) const
    {
        const std::scoped_lock locked(m_said->mutex);
        qint64 most = 0;
        for (const auto& [bytes, saidNs] : m_said->all) {
            if (saidNs < atNs) {
                most = std::max(most, bytes);
            }
        }
        return most;
    }
    qint64 inAll() const { return before(std::numeric_limits<qint64>::max()); }

private:
    struct Said {
        std::mutex mutex;
        std::vector<std::pair<qint64, qint64>> all; // (bytes, when)
    };
    std::shared_ptr<Said> m_said = std::make_shared<Said>();
};

// Whether the far end hears the line reset within 3 s, while this thread neither turns its loop nor touches the line:
// so where it does, the line's own thread reset it, of its own accord (this thread's looks, say() or the line's watch,
// would end the line too, and have that thread reset it then).
bool resetBeforeThisThreadLooks(const FarEndOnItsOwnThread& device)
{
    for (int waited = 0; device.end().isEmpty() && waited < 3000; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return device.end() == "reset"_ba;
}

// What Qt's loop on Windows says of a socket's message (qeventdispatcher_win.cpp: WM_QT_SOCKETNOTIFIER, which is
// WM_USER, with Winsock's FD_ event in the low word of lParam): whether `message` is that, for that event. For a test
// that puts a line's thread aside at one such message (lines::afterALook).
bool socketSays(const void* message, int event)
{
    const auto* const msg = static_cast<const MSG*>(message);
    return msg != nullptr && msg->message == static_cast<UINT>(WM_USER) &&
           static_cast<int>(WSAGETSELECTEVENT(msg->lParam)) == event;
}

// A JetKVM's signalling line, as its driver asks for it.
DeviceLine::Where signalling(const QUrl& address)
{
    DeviceLine::Where where;
    where.address = address;
    where.asking = "/webrtc/signaling/client"_ba;
    where.cookie = "authToken=a.pass-1"_ba;
    return where;
}

DeviceLine::End endOf(const QSignalSpy& ended)
{
    return ended.first().first().value<DeviceLine::End>();
}

} // namespace

class TestDeviceLine : public QObject {
    Q_OBJECT

private slots:
    void aLineThatReadsTextWithoutASubprotocolHearsWhatTheDeviceSays()
    {
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy text(&line, &DeviceLine::textCame);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        // (It asked for none.)
        QVERIFY(!device.asking.toLower().contains("sec-websocket-protocol"));
        QVERIFY(device.asking.contains("\r\nCookie: authToken=a.pass-1\r\n"));
        device.send('\x81', R"({"type":"device-metadata"})"_ba);
        QTRY_COMPARE(text.size(), 1);
        QCOMPARE(text.first().first().toByteArray(), R"({"type":"device-metadata"})"_ba);
    }

    void aLineThatReadsNoTextThrowsItAway()
    {
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy text(&line, &DeviceLine::textCame);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x81', "said"_ba);
        QTest::qWait(200);
        QVERIFY(text.isEmpty());
        QVERIFY(line.isIn());
    }

    void aNoSaysWhetherWhoeverAsksIsNotSignedInOrMayNotHaveTheLine_data()
    {
        // (The web's lines: a kvmd KVM signs in anew on either, a JetKVM only where it is not signed in.)
        QTest::addColumn<QByteArray>("answer");
        QTest::addColumn<DeviceLine::End>("end");
        QTest::newRow("not signed in (401)")
            << "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"_ba << DeviceLine::End::SignedOut;
        QTest::newRow("may not have it (403)")
            << "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"_ba << DeviceLine::End::Refused;
        QTest::newRow("anything else (404)")
            << "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"_ba << DeviceLine::End::NotLetIn;
    }

    void aNoSaysWhetherWhoeverAsksIsNotSignedInOrMayNotHaveTheLine()
    {
        QFETCH(QByteArray, answer);
        QFETCH(DeviceLine::End, end);
        HandMadeLine device;
        device.instead = answer;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), end);
        QVERIFY(in.isEmpty());
    }

    void whatNobodyAskedForEndsALineThatReadsTextWithoutASubprotocol_data()
    {
        QTest::addColumn<QByteArray>("besides");
        QTest::newRow("a subprotocol")
            << "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: janus-protocol\r\n"_ba;
        QTest::newRow("an extension")
            << "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Extensions: permessage-deflate\r\n"_ba;
        QTest::newRow("no word that it upgrades") << "Connection: Upgrade\r\n"_ba;
    }

    void whatNobodyAskedForEndsALineThatReadsTextWithoutASubprotocol()
    {
        QFETCH(QByteArray, besides);
        HandMadeLine device;
        device.besides = besides;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotLetIn);
        QVERIFY(in.isEmpty());
    }

    void bytesOrTooMuchTextEndALineThatReadsText_data()
    {
        QTest::addColumn<char>("kind");
        QTest::addColumn<QByteArray>("message");
        QTest::newRow("bytes") << '\x82' << "\x01\x02"_ba;
        QTest::newRow("a text longer than may be") << '\x81' << QByteArray(301, 'a');
    }

    void bytesOrTooMuchTextEndALineThatReadsText()
    {
        QFETCH(char, kind);
        QFETCH(QByteArray, message);
        HandMadeLine device;
        // (Room for the answer that lets it in, which is read through the same buffer.)
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 300;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send(kind, message);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aTextInPartsIsHeardWholeAndHeldToTheLimitWhole_data()
    {
        QTest::addColumn<int>("first");
        QTest::addColumn<int>("second");
        QTest::addColumn<bool>("heard");
        // (Each part within the limit of 300: only the whole of the two is over it in the second row.)
        QTest::newRow("within the limit") << 150 << 100 << true;
        QTest::newRow("over the limit together") << 200 << 200 << false;
    }

    void aTextInPartsIsHeardWholeAndHeldToTheLimitWhole()
    {
        QFETCH(int, first);
        QFETCH(int, second);
        QFETCH(bool, heard);
        HandMadeLine device;
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 300;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy text(&line, &DeviceLine::textCame);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x01', QByteArray(first, 'a'));  // text, more to come
        device.send('\x80', QByteArray(second, 'b')); // the rest of it
        if (heard) {
            QTRY_COMPARE(text.size(), 1);
            QCOMPARE(text.first().first().toByteArray(), QByteArray(first, 'a') + QByteArray(second, 'b'));
            QVERIFY(ended.isEmpty());
        } else {
            QTRY_COMPARE(ended.size(), 1);
            QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
            QVERIFY(text.isEmpty());
        }
    }

    void aLimitSmallerThanTheAnswerThatLetsItInStillLetsItIn()
    {
        // (What may wait to be read is never less than that answer, whatever a message may be.)
        HandMadeLine device;
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 32;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x81', QByteArray(33, 'a'));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aLineThatReadsTextIsNoServersLiveLine()
    {
        // Asked to be one after it was asked to read text, it stays what it was: bytes end it.
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        line.receiveText();
        line.receiveLive();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QVERIFY(device.asking.contains("\r\nCookie: authToken=a.pass-1\r\n"));
        device.send('\x82', "\x01\x02"_ba);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aLineAskedForWithoutACookieCarriesNone()
    {
        HandMadeLine device;
        DeviceLine::Where where = signalling(device.address());
        where.cookie.clear();
        where.withoutCookie = true;
        DeviceLine line(where, {});
        line.receiveText();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QVERIFY(!device.asking.toLower().contains("cookie"));
        QVERIFY(!device.asking.toLower().contains("x-qkvm"));
    }

    void aLineIsAskedForWithACookieOrExpresslyWithoutOne_data()
    {
        QTest::addColumn<QByteArray>("cookie");
        QTest::addColumn<bool>("withoutCookie");
        QTest::addColumn<QByteArray>("asking");
        const QByteArray line = "/webrtc/signaling/client"_ba;
        QTest::newRow("none, and not said to be without") << QByteArray() << false << line;
        QTest::newRow("one, though said to be without") << "authToken=a.pass-1"_ba << true << line;
        QTest::newRow("none, as said, with an asking that splits")
            << QByteArray() << true << "/webrtc/signaling/client\r\nX-More: 1"_ba;
        QTest::newRow("none, as said, with an asking that is no path") << QByteArray() << true << "webrtc"_ba;
    }

    void aLineIsAskedForWithACookieOrExpresslyWithoutOne()
    {
        QFETCH(QByteArray, cookie);
        QFETCH(bool, withoutCookie);
        QFETCH(QByteArray, asking);
        HandMadeLine device;
        DeviceLine::Where where = signalling(device.address());
        where.cookie = cookie;
        where.withoutCookie = withoutCookie;
        where.asking = asking;
        DeviceLine line(where, {});
        line.receiveText();
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotAsked);
        QTest::qWait(100);
        QVERIFY(device.asking.isEmpty());
    }

    // --- a raw line: a VNC server's RFB straight on its TCP connection ----------------------------------------------

    void aRawLineIsInAtOnceAndCarriesBytesAsTheyAre()
    {
        RawPeer peer;
        DeviceLine::Where where;
        where.address = peer.address();
        DeviceLine line(where, {});
        line.receiveRaw();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        QByteArray came;
        connect(&line, &DeviceLine::bytesCame, this, [&came](const QByteArray& bytes) { came += bytes; });
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QTest::qWait(100);
        QCOMPARE(peer.heard, QByteArray()); // (nothing asked: no WebSocket, no header, no word of its own)
        peer.say("RFB 003.008\n"_ba);
        QTRY_COMPARE(came, "RFB 003.008\n"_ba);
        QVERIFY(line.sayBytes("RFB 003.008\n"_ba));
        QVERIFY(line.say("\x01"_ba));
        QTRY_COMPARE(peer.heard, "RFB 003.008\n\x01"_ba); // (as it is, without a frame)
        QVERIFY(!line.ping());
        QVERIFY(ended.isEmpty());
    }

    void aRawLineHandsOutWhatComesInPiecesOfItsLargestAndHoldsItToItsSecond()
    {
        RawPeer peer;
        DeviceLine::Where where;
        where.address = peer.address();
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 1024;
        limits.mostBytesASecond = 6000;
        DeviceLine line(where, limits);
        line.receiveRaw();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        qint64 total = 0;
        qsizetype largest = 0;
        connect(&line, &DeviceLine::bytesCame, this, [&total, &largest](const QByteArray& bytes) {
            total += bytes.size();
            largest = std::max(largest, bytes.size());
        });
        line.open();
        QTRY_COMPARE(in.size(), 1);
        peer.say(QByteArray(5000, 'x'));
        QTRY_COMPARE(total, qint64{5000});
        QVERIFY(largest <= 1024);
        QVERIFY(ended.isEmpty());
        // More in a second than it may take: over.
        peer.say(QByteArray(7000, 'y'));
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aRawLineClosedFromHereGoesAfterWhatWasSaid_data()
    {
        QTest::addColumn<int>("words");
        QTest::newRow("a few words") << 1;
        // (More than Windows takes at once while the far end reads nothing: the rest waits on the line's socket's
        // thread, which gives its socket one at a time, and ends the connection behind the last.)
        QTest::newRow("far more than Windows takes at once") << 400;
    }

    void aRawLineClosedFromHereGoesAfterWhatWasSaid()
    {
        QFETCH(int, words);
        RawPeer peer;
        DeviceLine::Where where;
        where.address = peer.address();
        DeviceLine::Limits limits;
        limits.goodbyeWithinMs = 4000; // (ended by itself at once: not by the reset after this)
        limits.mostUnsentBytes = qint64{4} * 1024 * 1024;
        limits.lateAfterMs = 5000; // (nor as late: the far end reads on this thread, a turn of its loop at a time)
        DeviceLine line(where, limits);
        line.receiveRaw();
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy gone(&line, &DeviceLine::gone);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QByteArray said;
        for (int one = 0; one < words; ++one) {
            const QByteArray word = one + 1 == words ? "last words"_ba : QByteArray(1000, 'w');
            QVERIFY(line.sayBytes(word));
            said += word;
        }
        QVERIFY(line.close());
        QCOMPARE(gone.size(), 0); // (never from inside close())
        QTRY_VERIFY_WITH_TIMEOUT(peer.ended, 3000);
        QCOMPARE(peer.heard.size(), said.size());
        QCOMPARE(peer.heard, said);
        QTRY_COMPARE(gone.size(), 1);
        QVERIFY(ended.isEmpty());
    }

    void aRawLineToNobodyIsNotReached()
    {
        quint16 nobody = 0;
        {
            QTcpServer server;
            server.listen(QHostAddress::LocalHost);
            nobody = server.serverPort();
        }
        DeviceLine::Where where;
        where.address = QUrl(u"http://127.0.0.1:%1"_s.arg(nobody));
        DeviceLine line(where, {});
        line.receiveRaw();
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotReached);
    }

    void aRawLineNotLetInInItsTimeIsSilent()
    {
        // Taken, and its TLS never answered: a line that did not answer in time, not one to nobody (outside review).
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        QList<QTcpSocket*> held;
        connect(&silent, &QTcpServer::newConnection, this, [&silent, &held] {
            while (QTcpSocket* one = silent.nextPendingConnection()) {
                held.append(one);
            }
        });
        DeviceLine::Where where;
        where.address = QUrl(u"https://127.0.0.1:%1"_s.arg(silent.serverPort()));
        DeviceLine::Limits limits;
        limits.letInWithinMs = 600;
        DeviceLine line(where, limits);
        line.receiveRaw();
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Silent);
        QVERIFY(!held.isEmpty());
    }

    // --- a line that reads bytes: a management controller's console (AMI's, at /kvm) ----------------------------

    void aLineThatReadsBytesHearsThemWholeAndSaysWhatItIsAndWhereFrom()
    {
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Where where = signalling(device.address());
        where.asking = "/kvm"_ba;
        where.cookie = "QSESSIONID=a1b2c3"_ba;
        DeviceLine line(where, {});
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy bytes(&line, &DeviceLine::bytesCame);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        QVERIFY(device.asking.contains("\r\nSec-WebSocket-Protocol: binary\r\n"));
        QVERIFY(device.asking.contains("\r\nOrigin: " + device.address().toString().toLatin1() + "\r\n"));
        QVERIFY(device.asking.contains("\r\nCookie: QSESSIONID=a1b2c3\r\n"));
        device.send('\x82', "\x17\x00\x00\x00\x00\x00\x02\x00"_ba);
        QTRY_COMPARE(bytes.size(), 1);
        QCOMPARE(bytes.first().first().toByteArray(), "\x17\x00\x00\x00\x00\x00\x02\x00"_ba);
        // ...and one in parts, whole.
        device.send('\x02', QByteArray(300, 'a'));
        device.send('\x00', QByteArray(300, 'b'));
        device.send('\x80', "end"_ba);
        QTRY_COMPARE(bytes.size(), 2);
        QCOMPARE(bytes.last().first().toByteArray(), QByteArray(300, 'a') + QByteArray(300, 'b') + "end"_ba);
        QVERIFY(ended.isEmpty());
    }

    void aLineHeldBackTakesNothingMoreUntilItIsLetGo_data()
    {
        QTest::addColumn<bool>("overTls");
        QTest::newRow("in the clear") << false;
        // (A server's console is over TLS: what waits is held by Qt hidden and in the clear, and by Windows, there.)
        QTest::newRow("over TLS") << true;
    }

    void aLineHeldBackTakesNothingMoreUntilItIsLetGo()
    {
        // (Review 3's HIGH 1, 2026-11-06: a server's console whose frames wait to be read on a thread of their own is
        // held back, as it was while the interface thread read them.) Held back from inside what it hands out, a line
        // hands out nothing more: what comes waits, in here no more than the line takes of one message, the rest with
        // Windows and then with whoever sends it. Let go, all of it comes in its order, a turn of the loop later.
        QFETCH(bool, overTls);
        HandMadeLine device(overTls);
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Limits limits;
        limits.mostBytesASecond = qint64{64} * 1024 * 1024; // (as much as it may send: a console's)
        limits.mostMessagesASecond = 20000;
        DeviceLine::Where where = signalling(device.address());
        if (overTls) {
            where.knownByCertificate = true;
            where.certificate = firstsSha256();
        }
        DeviceLine line(where, limits);
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        QList<QByteArray> came;
        connect(&line, &DeviceLine::bytesCame, this, [&came, &line](const QByteArray& bytes) {
            came.append(bytes.first(std::min<qsizetype>(bytes.size(), 8)));
            if (came.size() == 1) {
                line.holdBack(true);
            }
        });
        line.open();
        QTRY_COMPARE(in.size(), 1);
        constexpr int messages = 512; // (of 64000 bytes each: 32 MB)
        for (int one = 0; one < messages; ++one) {
            device.send('\x82', QByteArray::number(one).rightJustified(8, '0') + QByteArray(64000 - 8, 'x'));
        }
        QTRY_COMPARE(came.size(), 1);
        QTest::qWait(400);
        QCOMPARE(came.size(), 1);
        QVERIFY2(device.waiting() > qint64{16} * 1024 * 1024, QByteArray::number(device.waiting()));
        line.holdBack(false);
        QCOMPARE(came.size(), 1); // (not from inside)
        QTRY_COMPARE_WITH_TIMEOUT(came.size(), messages, 20000);
        for (int one = 0; one < messages; ++one) {
            QCOMPARE(came.at(one), QByteArray::number(one).rightJustified(8, '0'));
        }
        QVERIFY(ended.isEmpty());
    }

    void roomTheLineGivesBackWhileItsThreadFindsNoneIsNotMissed()
    {
        // (Outside review 2, 2.1.) What the line holds of what came fills all its room, and what its socket's thread
        // took last left nothing in the socket. More comes: that thread finds no room for it, and is put aside right
        // there (lines::afterTheRoom), as Windows or a busier PC may put it aside. Meanwhile the line reads all it held
        // and gives its room back. When that thread goes on, what waits in its socket is taken, with nothing more sent:
        // whichever of the two looks last sees the other. (Looked at once and said after, the line found nothing said
        // yet and asked for nothing, and that thread then said it waited and was told nothing more: a VNC server that
        // waits for the next question sends nothing more, and a console stalled until its watch ended it.)
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device(true);
        DeviceLine::Limits limits;
        limits.largestFrameBytes = 1024; // (the room, then, is that of the longest answer that lets a line in)
        constexpr qint64 room = 8192 + 4;
        DeviceLine::Where where;
        where.address = device.address();
        DeviceLine line(where, limits);
        line.receiveRaw();
        line.holdBack(true);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        qint64 came = 0;
        connect(&line, &DeviceLine::bytesCame, this, [&came](const QByteArray& bytes) { came += bytes.size(); });
        line.open();
        QTRY_VERIFY(line.isIn());
        QTRY_VERIFY(device.accepted());
        // All the room there is, taken off the socket by that thread, and nothing left in it.
        device.send(QByteArray(room, 'r'));
        QTest::qWait(500);
        QCOMPARE(came, qint64{0});
        // More comes, and that thread finds no room for it: put aside there (1), until it is let go on (2).
        auto aside = std::make_shared<std::atomic<int>>(0);
        qkvm::core::lines::afterTheRoom([aside](qint64 found) {
            if (found > 0) {
                return false;
            }
            aside->store(1);
            for (int waited = 0; aside->load() == 1 && waited < 10'000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        });
        const auto noMore = qScopeGuard([aside] {
            aside->store(2);
            qkvm::core::lines::afterTheRoom({});
        });
        constexpr qint64 more = 100;
        device.send(QByteArray(more, 'm'));
        for (int waited = 0; aside->load() == 0 && waited < 3000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY2(aside->load() == 1, "what came after never found the line's thread without room");
        // The line reads all it held, and gives its room back, while that thread is aside.
        line.holdBack(false);
        QTRY_COMPARE(came, room);
        QTest::qWait(200);
        aside->store(2);
        QTRY_COMPARE_WITH_TIMEOUT(came, room + more, 3000);
        QVERIFY(ended.isEmpty());
    }

    void aLineOverTlsReadsOnWhereQtKeptAPieceOfARecordAtItsBound_data()
    {
        QTest::addColumn<bool>("roomFilled");
        QTest::newRow("with room to spare") << false;
        // (What Qt makes of what it takes from Windows again then waits for the line's room, as anything does.)
        QTest::newRow("its room filled to the byte") << true;
    }

    void aLineOverTlsReadsOnWhereQtKeptAPieceOfARecordAtItsBound()
    {
        // Over TLS the socket's bound holds for what came hidden as well, in Qt's own socket below the one the line's
        // thread reads. Qt takes no more from Windows once that one was full as Windows said more came, and takes again
        // once a read empties it; and Qt's TLS (Windows' own, in Qt 6.12) reads 16 KiB of it at a time and decrypts a
        // record at a time. Where what it read left there a piece of a record shorter than the rest of that record, it
        // waited for the rest, which nothing took from Windows: the line stood still with room to spare (seen under
        // load on the Windows on Arm test PC, as aLineHeldBackTakesNothingMoreUntilItIsLetGo over TLS, 14 bytes left
        // there: the bound is four times 16 KiB and 14). Here the device's records make that happen every time. The
        // line is held back until it holds all its room and Qt has decrypted as much again, its own bound, in records
        // sent one by one, so that Qt's TLS has nothing of a record left; then that socket below fills to its bound
        // with records the first of which is so long that Qt's fourth read leaves those 14 bytes with more of their
        // record to come, and Windows holds more; then the line is let go.
        QFETCH(bool, roomFilled);
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        HandMadeLine device(true);
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Limits limits;
        limits.mostBytesASecond = qint64{64} * 1024 * 1024; // (as much as it may send: a console's)
        limits.mostMessagesASecond = 20000;
        // The bound, as the line sets it (a whole frame of the largest message: readBufferFor), which is also the room
        // the line has, and Qt's own bound on what it decrypts; what Qt's TLS reads at a time, which is also the most a
        // record carries; and what is left of the bound once Qt has read that as often as it fits.
        const qint64 bound = limits.largestFrameBytes + 14;
        constexpr qint64 aRead = 16384;
        const qint64 reads = bound / aRead;
        const qint64 piece = bound % aRead;
        DeviceLine::Where where = signalling(device.address());
        where.knownByCertificate = true;
        where.certificate = firstsSha256();
        DeviceLine line(where, limits);
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        QList<QByteArray> came;
        connect(&line, &DeviceLine::bytesCame, this, [&came, &line](const QByteArray& bytes) {
            came.append(bytes.first(std::min<qsizetype>(bytes.size(), 8)));
            if (came.size() == 1) {
                line.holdBack(true);
            }
        });
        // What the line's thread does: each look at the room (as it takes what came, which over TLS is each time Qt has
        // decrypted something), each word of Windows' that more came, and the room it finds first once it goes on from
        // where it is put aside below.
        struct Seen {
            std::atomic<int> takings{0};
            std::atomic<int> moreCame{0};
            std::atomic<qint64> lastCameNs{0};
            std::atomic<bool> wentOn{false};
            std::atomic<qint64> roomThen{-1};
        };
        const auto seen = std::make_shared<Seen>();
        qkvm::core::lines::afterTheRoom([seen](qint64 room) {
            seen->takings.fetch_add(1);
            if (seen->wentOn.load()) {
                qint64 none = -1;
                seen->roomThen.compare_exchange_strong(none, room);
            }
            return false;
        });
        qkvm::core::lines::afterALook([seen](const void* message) {
            if (socketSays(message, FD_READ)) {
                seen->moreCame.fetch_add(1);
                seen->lastCameNs.store(nowNs());
            }
            return false;
        });
        const auto noMore = qScopeGuard([] {
            qkvm::core::lines::afterTheRoom({});
            qkvm::core::lines::afterALook({});
            qkvm::core::lines::beforeALook({});
        });
        line.open();
        QTRY_COMPARE(in.size(), 1);
        // A record the device makes of these alone, and how long it is on the line; and one sent once the one before
        // was decrypted and taken.
        const auto record = [&device](const QByteArray& bytes) {
            const qint64 before = device.hiddenSoFar();
            device.sendRaw(bytes);
            device.pushOut();
            return device.hiddenSoFar() - before;
        };
        const auto recordTaken = [&record, &seen](const QByteArray& bytes) {
            const int before = seen->takings.load();
            const qint64 onTheLine = record(bytes);
            const bool taken = QTest::qWaitFor([&seen, before] { return seen->takings.load() > before; }, 5000);
            return taken ? onTheLine : qint64{-1};
        };
        // The first message, alone in a record: the line hears it and is held back from inside. What that record is on
        // the line besides what it carries, every record of this line is (a header and a tag, and over TLS 1.3 a byte
        // that says what it is).
        const QByteArray first = "\x82\x08"_ba + QByteArray(8, '0');
        const qint64 firstOnTheLine = recordTaken(first);
        QVERIFY2(firstOnTheLine > 0, "the first record was never taken");
        QTRY_COMPARE(came.size(), 1);
        const qint64 besides = firstOnTheLine - first.size();
        // The records that fill the socket below: the first carries so much that Qt, having decrypted it after its
        // first read, has `left` bytes of the next; each full record after it, decrypted as Qt reads 16 KiB more,
        // leaves `besides` fewer, so that Qt's last read of 16 KiB falls short of its record by more than the piece.
        const qint64 left = ((reads - 2) * besides) + ((besides - piece) / 2);
        const qint64 firstCarries = aRead - left - besides;
        QVERIFY2(reads >= 3 && besides > piece && besides < 256, QByteArray::number(besides));
        // What Qt decrypts of those before it waits for the rest, and what the line holds then of the message under
        // way: so much that that fills its room to the byte, or a thousand bytes.
        const qint64 decrypted = firstCarries + ((reads - 2) * aRead);
        const qint64 holds = roomFilled ? bound - decrypted : 1000;
        QVERIFY(holds > 0 && holds + decrypted <= bound);
        // How many full records the line takes its room of and Qt decrypts its bound of (it decrypts nothing more once
        // its decrypted buffer holds that much); where they end; and what the device sends: messages of bytes, each
        // beginning with its number, in frames that say their length in two bytes (130 to 65539 bytes), the one under
        // way there beginning `holds` bytes before, and much more behind it.
        const qint64 fullRecords = ((2 * bound) + aRead - 1) / aRead;
        const qint64 at = first.size() + (fullRecords * aRead);
        QByteArray stream = first;
        int messages = 1;
        const auto add = [&stream, &messages](qsizetype frame) {
            const qsizetype length = frame - 4;
            stream.append('\x82');
            stream.append(static_cast<char>(126));
            stream.append(static_cast<char>((length >> 8) & 0xFF));
            stream.append(static_cast<char>(length & 0xFF));
            stream.append(QByteArray::number(messages).rightJustified(8, '0'));
            stream.append(QByteArray(length - 8, 'x'));
            ++messages;
        };
        while (stream.size() < at - holds) {
            const qint64 toGo = at - holds - stream.size();
            add(toGo >= 16004 + 130 ? 16004 : toGo);
        }
        QCOMPARE(stream.size(), at - holds);
        add(60004);
        while (stream.size() < at + (6 * bound)) {
            add(16004);
        }
        for (qint64 one = 0; one < fullRecords; ++one) {
            const qint64 onTheLine = recordTaken(stream.mid(first.size() + (one * aRead), aRead));
            QCOMPARE(onTheLine, aRead + besides);
        }
        // Then the records that fill the socket below, and more: Qt decrypts none of them, and once Windows says more
        // came while that socket is full, it takes no more from Windows and hears nothing more of it.
        const int takenBefore = seen->takings.load();
        const int cameBefore = seen->moreCame.load();
        QCOMPARE(record(stream.mid(at, firstCarries)), firstCarries + besides);
        device.sendRaw(stream.sliced(at + firstCarries));
        device.pushOut();
        QVERIFY2(QTest::qWaitFor(
                     [&seen, cameBefore] {
                         return seen->moreCame.load() >= cameBefore + 2 &&
                                nowNs() - seen->lastCameNs.load() > qint64{300'000'000};
                     },
                     10'000),
                 "Windows went on saying that more came");
        QCOMPARE(seen->takings.load(), takenBefore);
        QCOMPARE(came.size(), 1);
        // Let go, the line reads what it held and gives its room back, and its thread takes all Qt had decrypted; that
        // read has Qt look at the socket below by a call of its own, before which that thread is put aside until the
        // line's own thread has done with all it was handed (a call posted to it behind that has come back): then the
        // line holds `holds` bytes, and nothing waits for it, as Qt's TLS reads to the piece.
        qkvm::core::lines::beforeALook([seen](const void* message) {
            if (message != nullptr) {
                return false; // (a message of Windows')
            }
            const auto back = std::make_shared<std::atomic<bool>>(false);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [back] { back->store(true); }, Qt::QueuedConnection);
            for (int waited = 0; !back->load() && waited < 10'000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            seen->wentOn.store(true);
            return true;
        });
        line.holdBack(false);
        QTRY_VERIFY_WITH_TIMEOUT(seen->roomThen.load() >= 0, 10'000);
        QCOMPARE(seen->roomThen.load(), bound - holds);
        // ...and all of it comes, in its order. (Half a megabyte in all: here in a second at the most. A line that
        // stood still is said within the program's own time, with the twice as long that Qt then waits to say whether
        // more time would have done.)
        QTRY_COMPARE_WITH_TIMEOUT(came.size(), messages, 5000);
        for (int one = 0; one < messages; ++one) {
            QCOMPARE(came.at(one), QByteArray::number(one).rightJustified(8, '0'));
        }
        QVERIFY(ended.isEmpty());
    }

    void whatALineSaidLastArrivesWhereItIsThrownAwayAfterItsGoodbyeOverMoreThanItTook_data()
    {
        // More than this program takes off a line that it does not read: the line's socket's thread takes what comes
        // as it comes (outside review 2.2), as far as there is room for it here and in its socket (one message's worth
        // each, and over TLS once more hidden), and the rest lies with Windows.
        QTest::addColumn<bool>("overTls");
        QTest::addColumn<int>("kilobytes");
        QTest::newRow("in the clear") << false << 160;
        QTest::newRow("over TLS") << true << 224;
    }

    void whatALineSaidLastArrivesWhereItIsThrownAwayAfterItsGoodbyeOverMoreThanItTook()
    {
        // A line closed and thrown away in the same breath, over what the device had sent and nobody read: Windows
        // resets a line that is closed over what it holds unread, and a far end that is Windows too then loses what
        // it had not taken yet. So what lies on the line is taken off it as the socket goes, and the device gets what
        // was said last and the goodbye, and the line's end in order.
        QFETCH(bool, overTls);
        QFETCH(int, kilobytes);
        HandMadeLine device(overTls);
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Where where = signalling(device.address());
        if (overTls) {
            where.knownByCertificate = true;
            where.certificate = firstsSha256();
        }
        auto line = std::make_unique<DeviceLine>(where, DeviceLine::Limits{});
        line->receiveBytes("binary"_ba);
        const QSignalSpy in(line.get(), &DeviceLine::letIn);
        line->open();
        QTRY_COMPARE(in.size(), 1);
        const qsizetype heardBefore = device.afterAsking().size();
        for (int sent = 0; sent < kilobytes; sent += 32) {
            device.send('\x82', QByteArray(qsizetype{32} * 1024, 'k'));
        }
        device.pushOut();
        QThread::msleep(300); // (nothing of this thread's loop meanwhile: the line reads none of it)
        QVERIFY(line->sayBytes("last"_ba));
        QVERIFY(line->close());
        line.reset();
        QTRY_VERIFY_WITH_TIMEOUT(device.ended(), 10'000);
        // "last" (two bytes, four that hide it, four) and the goodbye (two, four, its two), each as a caller hides it.
        const QByteArray heard = device.afterAsking().mid(heardBefore);
        QCOMPARE(heard.size(), qsizetype{18});
        QCOMPARE(static_cast<quint8>(heard.at(0)), quint8{0x82});
        QCOMPARE(static_cast<quint8>(heard.at(10)), quint8{0x88});
    }

    void whatWaitedTooLongIsNotHandedOnWhereTheLinesOwnThreadWasHeldPastIt_data()
    {
        QTest::addColumn<bool>("muchWaits");
        QTest::addColumn<bool>("looked");
        QTest::newRow("behind much that waits") << true << true;
        QTest::newRow("where the one said last alone waits") << false << true;
        // (Outside review of tls, L3: the guards behind the look before Qt, on their own. Without that look, Qt hands
        // on what its socket holds at the first word of Windows' it acts on; that is one message, and it is found late
        // as it leaves.)
        QTest::newRow("behind much that waits, no look before Qt") << true << false;
        QTest::newRow("where the one said last alone waits, no look before Qt") << false << false;
    }

    void whatWaitedTooLongIsNotHandedOnWhereTheLinesOwnThreadWasHeldPastIt()
    {
        // (Outside review 2.6, where review 2.2 put the line's socket.) The line's socket's thread is held past
        // lateAfterMs, as Windows holds it while it judges the certificate again (at a session ticket of TLS 1.3, a key
        // update, a renegotiation), and meanwhile the way comes back: the far end takes all that Windows had for it,
        // and Windows has room again. When that thread goes on, Qt would hand what it holds to Windows at the first
        // turn of its loop, before this program's watch on what waits has its own turn: what that loop takes from
        // Windows is looked at first, and of what had waited too long nothing leaves; the line ends Late there and then
        // (reset, so that Windows throws away whatever it held for it). This thread is busy meanwhile too. Without that
        // look (`looked` false: lines::looksBeforeQt), no more than the one message the socket was given leaves (it is
        // given one at a time, and only while it holds nothing), and it ends the line Late as it leaves. (Outside
        // review 2, 2.3: what went to Windows is seen where that thread hands it on, HandedToWindows, and the far end
        // is held to that by count; and that the line was found late on that thread, by the far end's reset before
        // this thread does anything with the line, whose own looks would find it late too.)
        QFETCH(bool, muchWaits);
        QFETCH(bool, looked);
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device;
        const HandedToWindows handed;
        DeviceLine::Limits limits;
        limits.lateAfterMs = 300;
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        DeviceLine line(signalling(device.address()), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        const qint64 asked = line.mark(); // (the asking: all that was said on it before it was let in)
        const int size = muchWaits ? 100 : 1000;
        const qint64 onTheLine = size + 8; // (its beginning, the four bytes that hide it, and itself, at the most)
        if (muchWaits) {
            for (int one = 0; one < 3000; ++one) {
                QVERIFY(line.say(QByteArray(size, 'k')));
            }
        } else {
            // One at a time, until one waits in this program: Windows took each before it. (The one that waits is
            // what the socket holds, the only one it was given: it is counted as waiting, and nothing is said behind
            // it, which is seen below.)
            for (int one = 0; one < 2000 && line.waitedMs() == 0; ++one) {
                QVERIFY(line.say(QByteArray(size, 'k')));
                QTest::qWait(5);
            }
            QVERIFY(line.waitedMs() > 0);
        }
        const qint64 said = line.mark();
        // Held there, past lateAfterMs.
        qkvm::core::lines::looksBeforeQt(looked);
        const auto lookAgain = qScopeGuard([] { qkvm::core::lines::looksBeforeQt(true); });
        const int heldMs = limits.lateAfterMs + 400;
        const std::vector<std::shared_ptr<Held>> holding = holdLinesThreads(heldMs, 1);
        QVERIFY(!holding.empty());
        const std::shared_ptr<Held>& held = holding.front();
        // The way comes back; and this thread busy until well after that thread has gone on.
        device.startReading();
        QThread::msleep(static_cast<unsigned long>(heldMs) + 300);
        QVERIFY(held->untilNs.load() != 0);
        // Found late on that thread as it went on, and reset there: the far end hears the reset while this thread has
        // done nothing with the line since. Then what is said on the line is not said.
        QVERIFY2(resetBeforeThisThreadLooks(device), qPrintable(u"the far end heard \"%1\""_s.arg(device.end())));
        QVERIFY(!line.say("x"_ba));
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 3000);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::Late);
        // What went to Windows after that thread went on: nothing after the look, one message at the most without it.
        // And the far end heard no more than had gone to Windows: after the look, no more than had gone before.
        const qint64 before = handed.before(held->untilNs.load());
        const qint64 late = handed.inAll() - before;
        const QString seen = u"%1 bytes handed to Windows after that thread went on, %2 before it, of %3 said; %4 "
                             "reached the far end after the asking"_s.arg(late)
                                 .arg(before)
                                 .arg(said)
                                 .arg(device.heard());
        qInfo("%s", qPrintable(seen));
        QVERIFY2(looked ? late == 0 : late <= onTheLine, qPrintable(seen));
        QVERIFY2(device.heard() <= (looked ? before : handed.inAll()) - asked, qPrintable(seen));
        if (!muchWaits) {
            // All but the one the socket held went to Windows before the hold (and some of that one, where Windows had
            // room for part of it), and after the look nothing more. (Without the look, the rest of it may have gone,
            // late, as it ended the line.)
            const qint64 missing = said - handed.inAll();
            QVERIFY2((looked ? missing > 0 : missing >= 0) && missing <= onTheLine, qPrintable(seen));
        }
    }

    void aLineThatTakesNothingHoldsTheDeviceBackWhileItsUserIsBusy_data()
    {
        QTest::addColumn<bool>("shortTexts");
        QTest::addColumn<bool>("overTls");
        // (Outside review of tls, M1: kvmd's console line reads nothing, its awake line short texts alone.)
        QTest::newRow("a line that reads nothing, in the clear") << false << false;
        QTest::newRow("a line that reads nothing, over TLS") << false << true;
        QTest::newRow("a line that hears short texts, in the clear") << true << false;
        QTest::newRow("a line that hears short texts, over TLS") << true << true;
    }

    void aLineThatTakesNothingHoldsTheDeviceBackWhileItsUserIsBusy()
    {
        // A device floods a line that throws away what comes, while the thread the line lives on is busy: the line's
        // socket's thread takes no more off the line than the line has room for (a message's worth there and one
        // here), and the rest waits with Windows and then with the device, which is held back by that, as it was while
        // the socket was on this thread. Then the line takes what comes, and ends as more than a KVM has reason to send
        // in a second ends it.
        QFETCH(bool, shortTexts);
        QFETCH(bool, overTls);
        HandMadeLineElsewhere device(overTls);
        DeviceLine::Where where = signalling(device.address());
        if (overTls) {
            where.knownByCertificate = true;
            where.certificate = firstsSha256();
        }
        DeviceLine line(where, {});
        if (shortTexts) {
            line.hearShortTexts(512);
        }
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        constexpr int messages = 512; // (of 64000 bytes each: 32 MB)
        constexpr qint64 sent = qint64{messages} * (64000 + 4);
        device.sendMany(messages, 64000);
        QThread::msleep(2000); // (nothing of this thread's loop meanwhile)
        const qint64 waiting = device.waiting();
        QVERIFY2(waiting > sent * 3 / 4,
                 qPrintable(u"of %1 bytes sent, %2 still waited with the device"_s.arg(sent).arg(waiting)));
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 10'000);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void whatWaitedTooLongIsNotHandedOnWhereTheLinesThreadIsPutAsideAfterItsLook_data()
    {
        QTest::addColumn<bool>("muchWaits");
        QTest::newRow("behind much that waits") << true;
        QTest::newRow("where the one said last alone waits") << false;
    }

    void whatWaitedTooLongIsNotHandedOnWhereTheLinesThreadIsPutAsideAfterItsLook()
    {
        // (Outside review of tls, L3.) As the case above, but the line's socket's thread is put aside after its look
        // at Windows' word that it has room again, before Qt hands on what the socket holds (lines::afterALook): as
        // Windows or a busier PC may put it aside, and no look of this program's can come closer to Qt's own work.
        // Of what waited too long, no more than the one message the socket was given can leave then (it is given one
        // at a time, and only while it holds nothing), and that one is found late as it leaves: the line ends Late
        // there, and is reset. (Seen as the case above sees it: outside review 2, 2.3.)
        QFETCH(bool, muchWaits);
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device;
        const HandedToWindows handed;
        DeviceLine::Limits limits;
        // (Long enough that nothing is late before that thread is put aside: much said in a breath takes a while on a
        // slow PC, and the look itself would find it late first, a road of the case above.)
        limits.lateAfterMs = 1500;
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        DeviceLine line(signalling(device.address()), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        const qint64 asked = line.mark(); // (the asking)
        const int size = muchWaits ? 100 : 1000;
        const qint64 onTheLine = size + 8; // (its beginning, the four bytes that hide it, and itself, at the most)
        if (muchWaits) {
            // (More than Windows takes on this PC's own address while the far end reads nothing: some waits here.)
            for (int one = 0; one < 3000; ++one) {
                QVERIFY(line.say(QByteArray(size, 'k')));
            }
            QTest::qWait(200);
            QVERIFY2(line.waitedMs() >= 150, QByteArray::number(line.waitedMs()).constData());
        } else {
            for (int one = 0; one < 2000 && line.waitedMs() == 0; ++one) {
                QVERIFY(line.say(QByteArray(size, 'k')));
                QTest::qWait(5);
            }
            QVERIFY(line.waitedMs() > 0);
        }
        // Put aside at Windows' word that it has room again, once the far end reads again: past lateAfterMs.
        const auto aside = std::make_shared<Held>();
        const int asideMs = limits.lateAfterMs + 400;
        qkvm::core::lines::afterALook([aside, asideMs](const void* message) {
            if (!socketSays(message, FD_WRITE)) {
                return false;
            }
            aside->fromNs = nowNs();
            QThread::msleep(static_cast<unsigned long>(asideMs));
            aside->untilNs = nowNs();
            return true;
        });
        const auto noMore = qScopeGuard([] { qkvm::core::lines::afterALook({}); });
        device.startReading();
        // (This thread busy until well after that thread went on: its own watch would find the line late first.)
        for (int waited = 0; aside->fromNs.load() == 0 && waited < 3000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY2(aside->fromNs.load() != 0, "Windows' word that it had room again never came to the line's thread");
        QThread::msleep(static_cast<unsigned long>(asideMs) + 300);
        QVERIFY(aside->untilNs.load() != 0);
        // Found late on that thread as it went on, and reset there, while this thread has done nothing with the line
        // since; then what is said on the line is not said.
        QVERIFY2(resetBeforeThisThreadLooks(device), qPrintable(u"the far end heard \"%1\""_s.arg(device.end())));
        QVERIFY(!line.say("x"_ba));
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 3000);
        QCOMPARE(ended.at(0).at(0).value<DeviceLine::End>(), DeviceLine::End::Late);
        // No more than the one message its socket held went to Windows after that thread went on; and the far end
        // heard no more than went to Windows.
        const qint64 late = handed.inAll() - handed.before(aside->untilNs.load());
        const QString seen = u"%1 bytes handed to Windows after that thread went on, of one message of %2 at the most; "
                             "%3 of %4 reached the far end after the asking"_s.arg(late)
                                 .arg(onTheLine)
                                 .arg(device.heard())
                                 .arg(handed.inAll() - asked);
        qInfo("%s", qPrintable(seen));
        QVERIFY2(late <= onTheLine, qPrintable(seen));
        QVERIFY2(device.heard() <= handed.inAll() - asked, qPrintable(seen));
    }

    void aLineCutWhileItsThreadIsPutAsideHandsNothingMoreOn()
    {
        // (Outside review of tls, L3 and L2.) The line's socket's thread is put aside at a message its loop took from
        // Windows (here: that the device sent something; lines::afterALook), and meanwhile the way comes back (Windows'
        // word that it has room again waits behind that message) and the line is cut here: the call that tells that
        // thread so may wait behind Windows' word. At whichever of the two comes first, the line is reset on its own
        // thread before Qt has it (at that word, by the look before Qt), so that what the socket held never goes:
        // nothing more of a line that ended is handed to Windows. (Seen where that thread hands it on, and the far end
        // held to that by count: outside review 2, 2.3. Which of the two comes first this case does not choose, as
        // Qt's loop keeps no one order for them: a removal of the look's own reset was caught in one of two runs. The
        // case below chooses it: aLineCutWhileItsThreadIsPutAsideBeforeItsLookHandsNothingMoreOn.)
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device;
        const HandedToWindows handed;
        DeviceLine::Limits limits;
        limits.lateAfterMs = 60'000; // (nothing is late here)
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        DeviceLine line(signalling(device.address()), limits);
        line.open();
        QTRY_VERIFY(line.isIn());
        const qint64 asked = line.mark(); // (the asking)
        for (int one = 0; one < 2000 && line.waitedMs() == 0; ++one) {
            QVERIFY(line.say(QByteArray(1000, 'k')));
            QTest::qWait(5);
        }
        QVERIFY(line.waitedMs() > 0);
        const auto aside = std::make_shared<Held>();
        qkvm::core::lines::afterALook([aside](const void* message) {
            if (!socketSays(message, FD_READ)) {
                return false;
            }
            aside->fromNs = nowNs();
            QThread::msleep(1000);
            aside->untilNs = nowNs();
            return true;
        });
        const auto noMore = qScopeGuard([] { qkvm::core::lines::afterALook({}); });
        device.send("\x81\x01x"_ba); // (a short text, which the line throws away)
        for (int waited = 0; aside->fromNs.load() == 0 && waited < 3000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY2(aside->fromNs.load() != 0, "Windows' word that something came never reached the line's thread");
        device.startReading();
        for (int waited = 0; device.heard() == 0 && waited < 500; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY(device.heard() > 0); // (what Windows held: its room is free again, and it says so behind)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        QVERIFY(aside->untilNs.load() == 0);
        line.cut();
        QTRY_COMPARE_WITH_TIMEOUT(device.end(), "reset"_ba, 5000);
        QVERIFY(aside->untilNs.load() != 0);
        const qint64 before = handed.before(aside->untilNs.load());
        const QString seen = u"%1 bytes handed to Windows after the line was cut, %2 before; %3 reached the far end "
                             "after the asking"_s.arg(handed.inAll() - before)
                                 .arg(before)
                                 .arg(device.heard());
        qInfo("%s", qPrintable(seen));
        QVERIFY2(handed.inAll() == before, qPrintable(seen));
        QVERIFY2(device.heard() <= before - asked, qPrintable(seen));
    }

    void aLineCutWhileItsThreadIsPutAsideBeforeItsLookHandsNothingMoreOn_data()
    {
        QTest::addColumn<bool>("cut");
        QTest::newRow("cut meanwhile") << true;
        // (Not cut, Qt hands on what the socket holds at that very word: the case puts that thread aside where Qt
        // would hand it on.)
        QTest::newRow("not cut") << false;
    }

    void aLineCutWhileItsThreadIsPutAsideBeforeItsLookHandsNothingMoreOn()
    {
        // (Review cx2-cxl, L1, of the outside review of tls, L2.) As the case above, but the line's socket's thread is
        // put aside at Windows' word that its socket has room again before the look before Qt looks at anything
        // (lines::beforeALook), and the line is cut here meanwhile: the call that tells that thread so waits behind
        // that word, so that only the look can reset the line there before Qt hands on what the socket holds. Nothing
        // more goes to Windows. (Seen where that thread hands it on, and the far end held to that by count.)
        QFETCH(bool, cut);
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device;
        const HandedToWindows handed;
        DeviceLine::Limits limits;
        limits.lateAfterMs = 60'000; // (nothing is late here)
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        DeviceLine line(signalling(device.address()), limits);
        line.open();
        QTRY_VERIFY(line.isIn());
        const qint64 asked = line.mark(); // (the asking)
        // One at a time, until one waits in this program: the socket holds it, and Windows has no room for it.
        for (int one = 0; one < 2000 && line.waitedMs() == 0; ++one) {
            QVERIFY(line.say(QByteArray(1000, 'k')));
            QTest::qWait(5);
        }
        QVERIFY(line.waitedMs() > 0);
        // Put aside at Windows' word that it has room again, once the far end reads, until it is let go on.
        const auto aside = std::make_shared<Held>();
        const auto goOn = std::make_shared<std::atomic<bool>>(false);
        qkvm::core::lines::beforeALook([aside, goOn](const void* message) {
            if (!socketSays(message, FD_WRITE)) {
                return false;
            }
            aside->fromNs = nowNs();
            for (int waited = 0; !goOn->load() && waited < 5000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            aside->untilNs = nowNs();
            return true;
        });
        const auto noMore = qScopeGuard([goOn] {
            goOn->store(true);
            qkvm::core::lines::beforeALook({});
        });
        device.startReading();
        for (int waited = 0; aside->fromNs.load() == 0 && waited < 3000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY2(aside->fromNs.load() != 0, "Windows' word that it had room again never came to the line's thread");
        if (cut) {
            line.cut();
        }
        goOn->store(true);
        for (int waited = 0; aside->untilNs.load() == 0 && waited < 3000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY(aside->untilNs.load() != 0);
        if (cut) {
            QTRY_COMPARE_WITH_TIMEOUT(device.end(), "reset"_ba, 5000);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(handed.inAll() > handed.before(aside->untilNs.load()), 3000);
        }
        const qint64 before = handed.before(aside->untilNs.load());
        const QString seen = u"%1 bytes handed to Windows after that thread went on, %2 before; %3 reached the far end "
                             "after the asking"_s.arg(handed.inAll() - before)
                                 .arg(before)
                                 .arg(device.heard());
        qInfo("%s", qPrintable(seen));
        if (cut) {
            QVERIFY2(handed.inAll() == before, qPrintable(seen));
        }
        QVERIFY2(device.heard() <= handed.inAll() - asked, qPrintable(seen));
    }

    void aLineThrownAwayNeitherClosedNorCutHandsNothingMoreOn_data()
    {
        QTest::addColumn<bool>("in");
        QTest::newRow("its asking, which says who asks") << false;
        QTest::newRow("what was said once it was let in") << true;
    }

    void aLineThrownAwayNeitherClosedNorCutHandsNothingMoreOn()
    {
        // (Outside review 2, 2.2.) A line is thrown away (its destructor: deleteLater, or its owner's end, as kvmd's
        // AwakeLine goes) neither closed nor cut, while its socket's thread is held, as Windows holds it while it
        // judges the certificate again a moment after the handshake, and something said on it waits for that thread:
        // the asking, with the cookie that says who asks in it (a Proxmox guest's API token, on its line), or what was
        // said once it was in. Nothing of that goes to Windows once the thread is let go: the line is reset there, as
        // a cut one is. (Before, that thread handed it all over then, with no time limit, and closed the line behind
        // it.)
        QFETCH(bool, in);
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device;
        const HandedToWindows handed;
        DeviceLine::Limits limits;
        limits.letInWithinMs = 10'000;
        limits.lateAfterMs = 60'000; // (nothing is late here)
        auto line = std::make_unique<DeviceLine>(signalling(device.address()), limits);
        line->open();
        if (in) {
            QTRY_VERIFY(line->isIn());
            device.startReading();
        } else {
            // That thread connects and tells this one, which does not turn its loop yet: the asking is made when it
            // does, and that thread is held by then.
            for (int waited = 0; !device.accepted() && waited < 5000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            QVERIFY(device.accepted());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        const qint64 before = line->mark();
        const std::vector<std::shared_ptr<Held>> holding = holdLinesThreads(1000, 1);
        QVERIFY(!holding.empty());
        if (in) {
            QVERIFY(line->say("a key"_ba));
        } else {
            QTRY_VERIFY(line->mark() > 0); // (the asking, handed to that thread)
        }
        QVERIFY(line->mark() > before);
        QVERIFY(holding.front()->untilNs.load() == 0); // (still held)
        line.reset();
        device.startReading();
        QTRY_VERIFY_WITH_TIMEOUT(!device.end().isEmpty(), 5000);
        QVERIFY(holding.front()->untilNs.load() != 0);
        const qint64 handedBefore = handed.before(holding.front()->untilNs.load());
        const QString seen = u"%1 bytes handed to Windows after that thread was let go, %2 before; of the asking %3 "
                             "came, and %4 after it"_s.arg(handed.inAll() - handedBefore)
                                 .arg(handedBefore)
                                 .arg(device.askingCame())
                                 .arg(device.heard());
        qInfo("%s", qPrintable(seen));
        QVERIFY2(handed.inAll() == handedBefore, qPrintable(seen));
        QVERIFY2(in ? device.heard() == 0 : device.askingCame() == 0, qPrintable(seen));
        QCOMPARE(device.end(), "reset"_ba);
    }

    void whatIsSaidBeforeALineIsLetInIsNotHeldToTheLateLimit()
    {
        // (Outside review of tls, L1.) The asking goes when the line's socket's thread takes it: where that thread is
        // held then (Windows judging the certificate again at a session ticket of TLS 1.3, a moment after the
        // handshake), the asking waits behind it. It is no input: it is not held to Limits::lateAfterMs, which would
        // end a sound line Late before it was let in, and again at every try (Limits::letInWithinMs bounds that part).
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000)); // (this line's alone)
        FarEndOnItsOwnThread device;
        const HandedToWindows handed;
        DeviceLine::Limits limits;
        limits.lateAfterMs = 300;
        limits.letInWithinMs = 10'000;
        DeviceLine line(signalling(device.address()), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        // The socket's thread connects and tells this thread, which does not turn its loop yet: the asking is made and
        // handed over when it does, and that thread is held by then.
        for (int waited = 0; !device.accepted() && waited < 5000; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY(device.accepted());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::vector<std::shared_ptr<Held>> holding = holdLinesThreads(limits.lateAfterMs + 700, 1);
        QVERIFY(!holding.empty());
        QTRY_VERIFY_WITH_TIMEOUT(line.isIn() || !ended.isEmpty(), 5000);
        QVERIFY2(ended.isEmpty(),
                 qPrintable(u"the line ended (%1) before it was let in"_s.arg(
                     ended.isEmpty() ? -1 : static_cast<int>(ended.at(0).at(0).value<DeviceLine::End>()))));
        // (As that thread handed it on: nothing before the hold's end, the asking after it. The far end's own time for
        // it is its thread's, after it read: outside review 2, 2.3.)
        QVERIFY2(handed.before(holding.front()->untilNs.load()) == 0 && handed.inAll() > 0,
                 "the asking did not wait behind the hold: the case did not come to what it is about");
        // And what is said once it is in is held to the limit as ever.
        QVERIFY(line.say("in"_ba));
        QVERIFY(line.isIn());
    }

    void whatIsSaidAfterTheDeviceEndedTheLineIsNeverTakenToHaveLeft()
    {
        // (Outside review of tls, L6.) The device ends the line, and its socket's thread hears of it before the line
        // does (this thread busy). What is said meanwhile is not handed to a socket that has ended, nor counted as
        // having left (hasLeft: a key let go of that never was); the line ends as one that fell.
        FarEndOnItsOwnThread device;
        DeviceLine::Limits limits;
        limits.lateAfterMs = 60'000; // (nothing is late here)
        DeviceLine line(signalling(device.address()), limits);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        QVERIFY(line.say("before"_ba));
        const qint64 before = line.mark();
        QTRY_VERIFY(line.hasLeft(before));
        device.endItsSide();
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // (nothing of this thread's loop meanwhile)
        QVERIFY(line.isIn());                                        // (not heard of here yet)
        QVERIFY(line.say("after its end"_ba));
        const qint64 after = line.mark();
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // (that thread took it meanwhile)
        QVERIFY2(!line.hasLeft(after), "what was said after the device ended the line was taken to have left");
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Fell);
    }

    void linesToAPlaceWhoseThreadsWindowsStillHoldsAreNotReachedUntilItLetsGo()
    {
        // (Outside review of tls, L7.) A line's thread that Windows holds (judging a certificate, waiting for a signer
        // far away) is let go of with its line, and ends when Windows lets go: each line opened meanwhile has another.
        // So once lines::mostHeldToOnePlace such threads to one place are held past lines::heldAfterMs, a line to that
        // place is not reached (End::NotReached, at once, and nothing goes there), while lines to other places go on;
        // once Windows lets go, the place is reached again. Held here as Windows holds them (lines::askEach).
        using qkvm::core::lines::heldAfterMs;
        using qkvm::core::lines::mostHeldToOnePlace;
        QVERIFY(QTest::qWaitFor([] { return qkvm::core::lines::running() == 0; }, 10'000));
        const auto aPlace = [this](QTcpServer& server, int& came) {
            QVERIFY(server.listen(QHostAddress::LocalHost));
            connect(&server, &QTcpServer::newConnection, this, [&server, &came] {
                while (QTcpSocket* const one = server.nextPendingConnection()) {
                    ++came;
                    one->setParent(&server);
                }
            });
        };
        QTcpServer held;
        QTcpServer other;
        int cameToHeld = 0;
        int cameToOther = 0;
        aPlace(held, cameToHeld);
        aPlace(other, cameToOther);
        const auto rawLineTo = [](const QTcpServer& server) {
            DeviceLine::Where where;
            where.address = QUrl(u"http://127.0.0.1:%1"_s.arg(server.serverPort()));
            auto line = std::make_unique<DeviceLine>(where, DeviceLine::Limits{});
            line->receiveRaw();
            return line;
        };
        const int heldMs = heldAfterMs + 4000;
        {
            std::vector<std::unique_ptr<DeviceLine>> lines;
            for (int one = 0; one < mostHeldToOnePlace; ++one) {
                lines.push_back(rawLineTo(held));
                lines.back()->open();
            }
            QTRY_VERIFY(std::ranges::all_of(lines, [](const auto& one) { return one->isIn(); }));
            QVERIFY(!holdLinesThreads(heldMs, mostHeldToOnePlace).empty());
        } // (let go of, their threads held)
        QTest::qWait(heldAfterMs + 300);
        QCOMPARE(qkvm::core::lines::running(), mostHeldToOnePlace);
        const int cameBefore = cameToHeld;
        {
            const auto refused = rawLineTo(held);
            const QSignalSpy ended(refused.get(), &DeviceLine::ended);
            refused->open();
            QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 1000);
            QCOMPARE(endOf(ended), DeviceLine::End::NotReached);
            QTest::qWait(200);
            QCOMPARE(cameToHeld, cameBefore); // (nothing went there)
        }
        {
            const auto elsewhere = rawLineTo(other);
            elsewhere->open();
            QTRY_VERIFY(elsewhere->isIn());
            QTRY_COMPARE(cameToOther, 1);
        }
        // Once Windows lets go of them, the place is reached again.
        QTRY_VERIFY_WITH_TIMEOUT(qkvm::core::lines::running() == 0, heldMs + 5000);
        const auto again = rawLineTo(held);
        again->open();
        QTRY_VERIFY(again->isIn());
    }

    void aRawLineClosedFromHereGoesAfterAllThatWaitsWhereTheFarEndTakesItAtOnce()
    {
        // (Outside review of tls, L3: R17.) A raw line closed with much said that waits on its socket's thread, and a
        // far end that takes what comes as it comes (on a thread of its own, so that Windows always has room): the
        // connection is ended behind the last of it. (Qt's socket in its closing state ends the connection the moment
        // it holds nothing, which here is after each message Windows takes whole: told to close at once, it would end
        // the line behind the first.)
        FarEndOnItsOwnThread device(true);
        DeviceLine::Where where;
        where.address = device.address();
        DeviceLine::Limits limits;
        limits.goodbyeWithinMs = 4000; // (ended by itself: not by the reset after this)
        limits.mostUnsentBytes = qint64{4} * 1024 * 1024;
        limits.lateAfterMs = 5000; // (nor as late: the far end reads nothing until the close)
        DeviceLine line(where, limits);
        line.receiveRaw();
        const QSignalSpy gone(&line, &DeviceLine::gone);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_VERIFY(line.isIn());
        qint64 said = 0;
        for (int one = 0; one < 400; ++one) {
            QVERIFY(line.sayBytes(QByteArray(1000, 'w')));
            said += 1000;
        }
        QVERIFY(line.close());
        device.startReading();
        QTRY_COMPARE_WITH_TIMEOUT(device.end(), "closed"_ba, 5000);
        QCOMPARE(device.heard(), said);
        QTRY_COMPARE(gone.size(), 1);
        QVERIFY(ended.isEmpty());
    }

    void aLineSaysHowLongWhatWasSaidHasWaitedToLeave()
    {
        // (For a server's console, where keys are not said behind what has waited too long, and the line goes on.)
        // What Windows takes of a line leaves at once as far as the line knows; what it does not take waits in the
        // line's program, and how long the oldest of it has waited is said.
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Where where = signalling(device.address());
        where.asking = "/kvm"_ba;
        where.cookie = "QSESSIONID=a1b2c3"_ba;
        DeviceLine::Limits limits;
        limits.mostUnsentBytes = qint64{64} * 1024 * 1024;
        limits.lateAfterMs = 600'000;
        DeviceLine line(where, limits);
        line.receiveBytes("binary"_ba);
        line.open();
        QTRY_VERIFY(line.isIn());
        QCOMPARE(line.waitedMs(), qint64{0});
        QVERIFY(line.sayBytes("x"_ba));
        const qint64 first = line.mark();
        QVERIFY(first > 0);
        QTRY_VERIFY(line.hasLeft(first));
        QCOMPARE(line.waitedMs(), qint64{0});
        device.stopReading();
        // Two megabytes: far more than Windows keeps for a line that nobody reads.
        const QByteArray words(1000, 'k');
        for (int one = 0; one < 2000; ++one) {
            QVERIFY(line.sayBytes(words));
        }
        const qint64 last = line.mark();
        QCOMPARE(last, first + qint64{2000} * (1000 + 8)); // (each a message of its own: its head, its mask, its bytes)
        QTest::qWait(300);
        QVERIFY2(line.waitedMs() >= 250, QByteArray::number(line.waitedMs()).constData());
        QVERIFY(line.hasLeft(first));
        QVERIFY(!line.hasLeft(last));
        QVERIFY(line.isIn());
        // A way that comes back takes it, and nothing waits any more.
        device.readAgain();
        QTRY_COMPARE_WITH_TIMEOUT(line.waitedMs(), qint64{0}, 10000);
        QVERIFY(line.hasLeft(last));
        QVERIFY(line.isIn());
    }

    void aLineThatReadsBytesEndsAtAText()
    {
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine line(signalling(device.address()), {});
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        device.send('\x81', "AAAA"_ba);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
    }

    void aLineThatReadsBytesIsNotLetInWithoutItsProtocol()
    {
        HandMadeLine device;
        DeviceLine line(signalling(device.address()), {});
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::NotLetIn);
        QVERIFY(in.isEmpty());
    }

    void aConsolesLineIsNoSignallingInTheVideoDiagnostics()
    {
        // (A review of the desktop's.) The video's diagnostics are the signalling's: a console's line that is not let
        // in, or ends, writes nothing there.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(u"video.jsonl"_s);
        {
            qkvm::core::VideoDiagnostics log(path);
            QVERIFY(log.isOpen());
            HandMadeLine refusing;
            DeviceLine notIn(signalling(refusing.address()), {});
            notIn.receiveBytes("binary"_ba);
            const QSignalSpy refused(&notIn, &DeviceLine::ended);
            notIn.open();
            QTRY_COMPARE(refused.size(), 1);
            HandMadeLine device;
            device.besides += "Sec-WebSocket-Protocol: binary\r\n";
            DeviceLine line(signalling(device.address()), {});
            line.receiveBytes("binary"_ba);
            const QSignalSpy in(&line, &DeviceLine::letIn);
            const QSignalSpy ended(&line, &DeviceLine::ended);
            line.open();
            QTRY_COMPARE(in.size(), 1);
            device.send('\x81', "AAAA"_ba);
            QTRY_COMPARE(ended.size(), 1);
        }
        QCOMPARE(QFileInfo(path).size(), qint64{0});
    }

    void aLineTakesAsManyMessagesASecondAsItIsToldItMay()
    {
        HandMadeLine device;
        device.besides += "Sec-WebSocket-Protocol: binary\r\n";
        DeviceLine::Limits limits;
        limits.mostMessagesASecond = 5;
        DeviceLine line(signalling(device.address()), limits);
        line.receiveBytes("binary"_ba);
        const QSignalSpy in(&line, &DeviceLine::letIn);
        const QSignalSpy bytes(&line, &DeviceLine::bytesCame);
        const QSignalSpy ended(&line, &DeviceLine::ended);
        line.open();
        QTRY_COMPARE(in.size(), 1);
        for (int one = 0; one < 8; ++one) { // (in one breath: in one second)
            device.send('\x82', "x"_ba);
        }
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(endOf(ended), DeviceLine::End::Misbehaved);
        QVERIFY(bytes.size() <= 5);
    }

    void aLineThatReadsBytesTakesWhateverComesInItsStride()
    {
        // (By seeded chance, the same at every run.) WebSocket's frames of every kind and none, of any length and
        // saying any length, whole, in parts and cut anywhere: what is handed out is a whole message no longer than
        // the line takes, and a line given what it does not take ends, for a reason there is.
        quint64 state = 2026;
        auto below = [&state](int limit) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<int>((state >> 33) % static_cast<quint64>(limit));
        };
        int handed = 0;
        int ends = 0;
        for (int round = 0; round < 40; ++round) {
            HandMadeLine device;
            device.besides += "Sec-WebSocket-Protocol: binary\r\n";
            DeviceLine::Limits limits;
            limits.largestFrameBytes = 4096;
            DeviceLine line(signalling(device.address()), limits);
            line.receiveBytes("binary"_ba);
            const QSignalSpy in(&line, &DeviceLine::letIn);
            const QSignalSpy bytes(&line, &DeviceLine::bytesCame);
            const QSignalSpy ended(&line, &DeviceLine::ended);
            line.open();
            QTRY_COMPARE(in.size(), 1);
            QByteArray stream;
            for (int frame = below(30); frame > 0; --frame) {
                static constexpr std::array<char, 8> firsts = {'\x82', '\x02', '\x00', '\x80',
                                                               '\x81', '\x89', '\x8A', '\xC2'};
                // (Mostly a whole message of bytes, which it takes, and now and then anything at all.)
                stream +=
                    below(3) != 0 ? '\x82' : firsts[static_cast<std::size_t>(below(static_cast<int>(firsts.size())))];
                const int length = below(8) == 0 ? below(70000) : below(300);
                const int said = below(40) == 0 ? below(70000) : length; // (now and then not what follows)
                if (said < 126) {
                    stream += static_cast<char>(said);
                } else if (said < 65536) {
                    stream += '\x7E';
                    stream += static_cast<char>(said >> 8);
                    stream += static_cast<char>(said);
                } else {
                    stream += '\x7F';
                    for (int shift = 56; shift >= 0; shift -= 8) {
                        stream += static_cast<char>(static_cast<quint64>(said) >> shift);
                    }
                }
                for (int one = 0; one < length; ++one) {
                    stream += static_cast<char>(below(256));
                }
            }
            if (below(3) == 0) {
                stream.truncate(below(static_cast<int>(stream.size()) + 1));
            }
            for (qsizetype at = 0; at < stream.size();) {
                const qsizetype step = 1 + below(5000);
                device.sendRaw(stream.mid(at, step));
                at += step;
            }
            QTest::qWait(150);
            for (const auto& one : bytes) {
                QVERIFY(one.first().toByteArray().size() <= limits.largestFrameBytes);
            }
            for (const auto& one : ended) {
                const auto why = one.first().value<DeviceLine::End>();
                QVERIFY(why == DeviceLine::End::Misbehaved || why == DeviceLine::End::Fell);
            }
            handed += static_cast<int>(bytes.size());
            ends += static_cast<int>(ended.size());
        }
        qInfo("40 lines: %d messages handed out, %d lines ended", handed, ends);
    }
};

QTEST_GUILESS_MAIN(TestDeviceLine)
#include "test_device_line.moc"
