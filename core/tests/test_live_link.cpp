#include <algorithm>
#include <array>
#include <atomic>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#include <memory>
#include <utility>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <qkvm/core/live_link.h>
#include <qkvm/core/server_client.h>

#include "live_stand_in.h"
#include "stand_in.h"

using namespace Qt::StringLiterals;
using namespace qkvm;
using core::LiveLink;
using core::testing::Answered;
using core::testing::Asked;
using core::testing::LiveStandIn;
using core::testing::StandIn;
using core::testing::Tls;

Q_DECLARE_METATYPE(qkvm::core::Live::Video)
Q_DECLARE_METATYPE(qkvm::core::Live::Picture)

namespace qkvm::core {
class LiveLinkTestAccess {
public:
    static const char* ticketData(const LiveLink& line) { return line.m_pass.constData(); }
    static void receiveDuringClose(LiveLink& line)
    {
        line.m_closing = true;
        line.take(QString::fromLatin1(R"({"t":"control","on":true})"));
        line.take(QString::fromLatin1(R"({"t":"state","state":"online","pointer":"both"})"));
        QByteArray picture(17, '\0');
        picture[0] = 2;
        line.take(picture);
    }
};
} // namespace qkvm::core
namespace {

#ifdef _DEBUG
class WhatIsGivenBack {
public:
    explicit WhatIsGivenBack(QByteArrayView secret)
    {
        Looking& now = looking();
        const qsizetype size = std::min<qsizetype>(secret.size(), static_cast<qsizetype>(now.bytes.size()));
        std::copy_n(secret.data(), size, now.bytes.begin());
        now.found.store(0);
        now.size.store(size);
        now.before = _CrtSetAllocHook(&WhatIsGivenBack::look);
    }
    ~WhatIsGivenBack()
    {
        Looking& now = looking();
        _CrtSetAllocHook(now.before);
        now.size.store(0);
    }
    WhatIsGivenBack(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack& operator=(const WhatIsGivenBack&) = delete;
    WhatIsGivenBack(WhatIsGivenBack&&) = delete;
    WhatIsGivenBack& operator=(WhatIsGivenBack&&) = delete;

    // How many blocks that were given back (or moved, as one that grows is) held it.
    static int holdingIt() { return looking().found.load(); }

private:
    struct Looking {
        std::array<char, 256> bytes{};
        std::atomic<qsizetype> size{0};
        std::atomic<int> found{0};
        _CRT_ALLOC_HOOK before = nullptr;
    };
    static Looking& looking()
    {
        static Looking one;
        return one;
    }
    static int __cdecl look(int what, void* block, size_t size, int use, long request, const unsigned char* file,
                            int line)
    {
        Looking& now = looking();
        const qsizetype length = now.size.load();
        if (length > 0 && block != nullptr && (what == _HOOK_FREE || what == _HOOK_REALLOC) &&
            (_BLOCK_TYPE(use) == _NORMAL_BLOCK || _BLOCK_TYPE(use) == _CLIENT_BLOCK)) {
            const size_t held = _msize_dbg(block, _BLOCK_TYPE(use));
            const auto* first = static_cast<const char*>(block);
            const auto* last = first + (held == static_cast<size_t>(-1) ? 0 : held);
            if (std::search(first, last, now.bytes.data(), now.bytes.data() + length) != last) {
                now.found.fetch_add(1);
            }
        }
        // (And it is given back, or moved, as it was to be.)
        return now.before != nullptr ? now.before(what, block, size, use, request, file, line) : 1;
    }
};

#endif

// A late ticket at the HTTP boundary. The production openLive callback still owns all
// allocation handoff and abandonment logic; no real server, device or credential is involved.
class TicketClient final : public core::ServerClient {
public:
    void askLive(const QString& machine, std::function<void(base::Result<QByteArray>)> done) override
    {
        askedMachine = machine;
        ServerClient::askLive(machine, [this, done = std::move(done)](base::Result<QByteArray> result) mutable {
            if (!result) {
                done(result.error());
                return;
            }
            allocation = QByteArrayView(result.value());
            // Pause only after the real HTTP/JSON path has produced its ticket.
            answer = [done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); };
        });
    }
    QString askedMachine;
    QByteArrayView allocation;
    std::function<void()> answer;
};

const QByteArray pass = "p4ss-for-one-line_0123456789";

// A server made by hand, for what one that speaks WebSocket properly cannot be made to do. It
// takes the call and never answers it; or answers it with what is no answer, and stays; or takes
// the line, as a QKVM would, and from then on answers nothing, not even the question whether it is
// still there. It can send a picture all the same, and it can stop taking what comes.
class SilentServer : public QObject {
public:
    enum class Does { NeverAnswers, AnswersRubbish, TakesTheLine };

    explicit SilentServer(Does does)
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this, does] {
            while (QTcpSocket* line = m_server.nextPendingConnection()) {
                m_lines.append(line);
                connect(line, &QTcpSocket::readyRead, this, [this, line, does] { hear(line, does); });
                connect(line, &QTcpSocket::disconnected, this, [this, line] {
                    m_lines.removeOne(line);
                    m_heard.remove(line);
                    m_answered.remove(line);
                    line->deleteLater();
                });
            }
        });
    }

    QByteArray upgradeHeaders = "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    QByteArray statusLine = "HTTP/1.1 101 Switching Protocols";
    QByteArray afterAccept;
    QString address() const { return u"http://127.0.0.1:%1"_s.arg(m_server.serverPort()); }
    // How many callers are still on a line of theirs.
    qsizetype callers() const { return m_lines.size(); }

    // From now on nothing that comes is taken off the line, and Windows keeps little of it.
    void stopReading()
    {
        m_reads = false;
        for (QTcpSocket* line : std::as_const(m_lines)) {
            line->setReadBufferSize(1024);
            line->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        }
    }

    // One picture, a JPEG, as a server sends one; and one word.
    void sendPicture(quint32 counter)
    {
        QByteArray message(16, '\0');
        message[0] = 2;
        qToLittleEndian<quint32>(counter, message.data() + 4);
        send('\x82', message + "a JPEG");
    }
    void sendWord() { send('\x81', R"({"t":"state","state":"online"})"); }
    void sendWire(const QByteArray& frames)
    {
        for (QTcpSocket* line : std::as_const(m_lines))
            line->write(frames);
    }

private:
    // One whole message, of bytes or of text, short enough for its length to be one byte.
    void send(char kind, const QByteArray& message)
    {
        QByteArray frame;
        frame.append(kind);
        frame.append(static_cast<char>(message.size()));
        for (QTcpSocket* line : std::as_const(m_lines)) {
            line->write(frame + message);
        }
    }

    void hear(QTcpSocket* line, Does does)
    {
        if (!m_reads) {
            return;
        }
        m_heard[line] += line->readAll();
        const QByteArray& heard = m_heard[line];
        if (does == Does::NeverAnswers || m_answered.contains(line) || !heard.contains("\r\n\r\n")) {
            return;
        }
        m_answered.insert(line);
        if (does == Does::AnswersRubbish) {
            line->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        const qsizetype at = heard.toLower().indexOf("sec-websocket-key:");
        const QByteArray key = heard.mid(at + 18, heard.indexOf("\r\n", at) - at - 18).trimmed();
        const QByteArray accept =
            QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
        line->write(statusLine + "\r\n" + upgradeHeaders + "Sec-WebSocket-Accept: " + accept + "\r\n" + afterAccept +
                    "\r\n");
    }

    QList<QTcpSocket*> m_lines;
    QHash<QTcpSocket*, QByteArray> m_heard;
    QSet<QTcpSocket*> m_answered;
    bool m_reads = true;
    // Last, and so the first to go, and its callers with it: while what they are noted in is
    // still there.
    QTcpServer m_server;
};

// While one of these is there, the program's lines trust what these makers of certificates signed,
// as they trust what the makers that Windows knows signed.
class VouchedFor {
public:
    explicit VouchedFor(const QString& makersFile) : m_before(QSslConfiguration::defaultConfiguration())
    {
        QSslConfiguration with = m_before;
        with.addCaCertificates(QSslCertificate::fromPath(makersFile, QSsl::Pem));
        QSslConfiguration::setDefaultConfiguration(with);
    }
    ~VouchedFor() { QSslConfiguration::setDefaultConfiguration(m_before); }
    VouchedFor(const VouchedFor&) = delete;
    VouchedFor& operator=(const VouchedFor&) = delete;
    VouchedFor(VouchedFor&&) = delete;
    VouchedFor& operator=(VouchedFor&&) = delete;

private:
    QSslConfiguration m_before;
};

// A line to a stand-in, opened by a client as the program opens one, and what it said.
struct Line {
    explicit Line(const QString& address, const QByteArray& knownCertificate = {}, LiveLink::Watch watch = {},
                  const QString& machine = u"a"_s)
    {
        static_cast<void>(client.setAddress(address));
        client.setKnownCertificate(knownCertificate);
        // A session there is, and it is not what the line is opened with.
        client.setSession("s1");
        link = client.openLive(machine, pass, LiveLink::Need{1280, 20}, watch);
        opened = std::make_unique<QSignalSpy>(link.get(), &LiveLink::opened);
        closed = std::make_unique<QSignalSpy>(link.get(), &LiveLink::closed);
        video = std::make_unique<QSignalSpy>(link.get(), &LiveLink::videoChanged);
        pictures = std::make_unique<QSignalSpy>(link.get(), &LiveLink::pictureCame);
        control = std::make_unique<QSignalSpy>(link.get(), &LiveLink::controlChanged);
        state = std::make_unique<QSignalSpy>(link.get(), &LiveLink::stateChanged);
    }

    LiveLink::Picture picture(qsizetype index) const { return pictures->at(index).at(0).value<LiveLink::Picture>(); }

    core::ServerClient client;
    std::unique_ptr<LiveLink> link;
    std::unique_ptr<QSignalSpy> opened;
    std::unique_ptr<QSignalSpy> closed;
    std::unique_ptr<QSignalSpy> video;
    std::unique_ptr<QSignalSpy> pictures;
    std::unique_ptr<QSignalSpy> control;
    std::unique_ptr<QSignalSpy> state;
};

Tls tlsOf(const char* name)
{
    const QString folder = QStringLiteral(QKVM_TESTDATA "/tls/");
    return {folder + QLatin1StringView(name) + u".crt"_s, folder + QLatin1StringView(name) + u".key"_s};
}

QByteArray sha256Of(const Tls& tls)
{
    return StandIn::certificateIn(tls.certificateFile).digest(QCryptographicHash::Sha256);
}

} // namespace

// One machine, live: the line to the server, what comes over it and what goes.
class TestLiveLink : public QObject {
    Q_OBJECT

private slots:
    void probeLiveUpgradeEdges_data()
    {
        QTest::addColumn<QByteArray>("status");
        QTest::addColumn<QByteArray>("after");
        QTest::addColumn<bool>("valid");
        QTest::newRow("second accept after the valid one")
            << QByteArray("HTTP/1.1 101 Switching Protocols")
            << QByteArray("Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n") << false;
        QTest::newRow("status 1010") << QByteArray("HTTP/1.1 1010 Switching Protocols") << QByteArray() << false;
        QTest::newRow("101 with empty reason") << QByteArray("HTTP/1.1 101 ") << QByteArray() << true;
        QTest::newRow("101 without reason or space") << QByteArray("HTTP/1.1 101") << QByteArray() << true;
    }
    void probeLiveUpgradeEdges()
    {
        QFETCH(QByteArray, status);
        QFETCH(QByteArray, after);
        QFETCH(bool, valid);
        SilentServer server(SilentServer::Does::TakesTheLine);
        server.statusLine = status;
        server.afterAccept = after;
        const Line line(server.address());
        QTRY_VERIFY(line.opened->count() + line.closed->count() > 0);
        QCOMPARE(line.opened->count(), valid ? 1 : 0);
        QCOMPARE(line.closed->count(), valid ? 0 : 1);
    }
    void incomingMessagesDuringCloseCannotRestoreControlOrPictures()
    {
        SilentServer server(SilentServer::Does::NeverAnswers);
        const Line line(server.address());
        core::LiveLinkTestAccess::receiveDuringClose(*line.link);
        QVERIFY(!line.link->hasControl());
        QCOMPARE(line.control->count(), 0);
        QCOMPARE(line.state->count(), 0);
        QCOMPARE(line.pictures->count(), 0);
        QCOMPARE(line.link->pointerMode(), core::Live::Pointer::Unknown);
    }
    void theClientTransfersItsTicketAllocationToTheLine()
    {
        SilentServer server(SilentServer::Does::NeverAnswers);
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()));
        QByteArray ticket(32, 'x');
        const QByteArrayView allocation(ticket);
        auto line = client.openLive(u"a"_s, std::move(ticket), {});
        // NOLINTNEXTLINE(bugprone-use-after-move): this regression checks what the handoff left behind.
        QVERIFY(ticket.isEmpty());
        QVERIFY(core::LiveLinkTestAccess::ticketData(*line) == allocation.data());
        line->close();
        QVERIFY(std::ranges::all_of(allocation, [](char c) { return c == '\0'; }));
    }
    void theAsyncClientTransfersTheTicketsOriginalAllocation()
    {
        StandIn server([](const Asked&) { return Answered{200, R"({"v":2,"ticket":"synthetic-async-ticket_2Rq4"})"}; });
        TicketClient client;
        QVERIFY(client.setAddress(server.address()));
        client.setSession("s1");
        std::unique_ptr<core::Live> line;
        client.openLive(u"a"_s, {}, [&](auto result) {
            QVERIFY(result);
            line = std::move(result).value();
            QVERIFY(core::LiveLinkTestAccess::ticketData(*static_cast<LiveLink*>(line.get())) ==
                    client.allocation.data());
        });
        QCOMPARE(client.askedMachine, u"a"_s);
        QVERIFY(!line);
        QTRY_VERIFY(client.answer != nullptr);
        client.answer();
        QVERIFY(line != nullptr);
        // Compare ownership BEFORE close, so a broken move never makes the test read freed memory.
        QVERIFY(core::LiveLinkTestAccess::ticketData(*static_cast<LiveLink*>(line.get())) == client.allocation.data());
        line->close();
        QVERIFY(std::ranges::all_of(client.allocation, [](char c) { return c == '\0'; }));
    }
    void theAsyncClientWipesATicketAbandonedAfterItsServerChanged()
    {
#ifndef _DEBUG
        QSKIP("The debug heap observes an abandoned ticket before free; the handoff runs in every preset.");
#else
        StandIn server(
            [](const Asked&) { return Answered{200, R"({"v":2,"ticket":"synthetic-abandoned-ticket_8Ty3"})"}; });
        core::testing::NobodyThere after;
        TicketClient client;
        QVERIFY(client.setAddress(server.address()));
        client.setSession("s1");
        bool answered = false;
        client.openLive(u"a"_s, {}, [&](auto result) {
            answered = true;
            QVERIFY(!result);
            QCOMPARE(result.error().code, core::ServerClient::unreachable);
        });
        QTRY_VERIFY(client.answer != nullptr);
        QVERIFY(client.setAddress(after.address()));
        int leaked = -1;
        {
            const WhatIsGivenBack watching(client.allocation);
            client.answer();
            leaked = WhatIsGivenBack::holdingIt();
        }
        QVERIFY(answered);
        QCOMPARE(leaked, 0);
#endif
    }
    void aSharedTicketRemainsTheCallersAndTheLinesCopyIsWipedOnDestruction()
    {
#ifndef _DEBUG
        QSKIP("The debug heap observes bytes before free; other presets check live allocations on close.");
#else
        SilentServer server(SilentServer::Does::NeverAnswers);
        QUrl address(server.address());
        address.setScheme(u"ws"_s);
        QByteArray ticket("synthetic-shared-ticket_7Qx2");
        const QByteArray retained = ticket;
        auto link = std::make_unique<LiveLink>(QNetworkRequest(address), std::move(ticket), LiveLink::Need{},
                                               QByteArray{}, LiveLink::Watch{});
        QVERIFY(core::LiveLinkTestAccess::ticketData(*link) != retained.constData());
        int leaked = 0;
        {
            const WhatIsGivenBack watching(retained);
            link.reset();
            leaked = WhatIsGivenBack::holdingIt();
        }
        QCOMPARE(leaked, 0);
        QCOMPARE(retained, QByteArray("synthetic-shared-ticket_7Qx2"));
#endif
    }
    void theLiveLineIgnoresAProxyForAnAddressQtWouldProxy()
    {
        const auto before = QNetworkProxy::applicationProxy();
        const auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(before); });
        int proxyCalls = 0;
        StandIn proxy([&](const Asked&) {
            ++proxyCalls;
            return Answered{502, {}, "text/plain"}; // Refuse the control CONNECT without an authentication retry.
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, u"127.0.0.1"_s, proxy.serverPort()));
        const core::testing::NobodyThere nobody;
        {
            QTcpSocket plain;
            plain.connectToHost(u"0.0.0.0"_s, nobody.port());
            QTRY_COMPARE_WITH_TIMEOUT(proxyCalls, 1, 3000);
        }
        proxyCalls = 0;
        LiveLink::Watch watch;
        watch.openWithinMs = 800;
        const Line line(u"http://0.0.0.0:%1"_s.arg(nobody.port()), {}, watch);
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 3000);
        QTest::qWait(200);
        QCOMPARE(proxyCalls, 0);
        QCOMPARE(line.opened->count(), 0);
    }
    void theEncryptedPinGuardWorksEvenWhenTlsVerificationIsDisabled()
    {
        const auto before = QSslConfiguration::defaultConfiguration();
        const auto restore = qScopeGuard([&] { QSslConfiguration::setDefaultConfiguration(before); });
        auto unverified = before;
        unverified.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(unverified);
        const auto shown = tlsOf("first");
        LiveStandIn server(shown.certificateFile, shown.keyFile);
        { // Control: this configuration really accepts an otherwise untrusted leaf.
            const Line accepted(server.address());
            QTRY_COMPARE(accepted.opened->count(), 1);
        }
        const int beforeLines = server.lines;
        const Line refused(server.address(), sha256Of(tlsOf("second")));
        QTRY_VERIFY(refused.opened->count() + refused.closed->count() > 0);
        QCOMPARE(refused.opened->count(), 0);
        QCOMPARE(refused.closed->count(), 1);
        QCOMPARE(server.lines, beforeLines);
    }
    void liveFramesEnforceBoundsAndFraming_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("bytes");
        QTest::addColumn<bool>("fragments");
        QTest::addColumn<bool>("valid");
        QTest::newRow("text over limit") << 1 << 65537 << false << false;
        QTest::newRow("fragmented text over limit") << 1 << 90000 << true << false;
        QTest::newRow("exact text limit") << 1 << 65536 << false << true;
        QTest::newRow("fragmented exact text limit") << 1 << 65536 << true << true;
        QTest::newRow("binary over limit") << 2 << int(core::largestPictureBytes + 17) << false << false;
        QTest::newRow("fragmented binary over limit") << 2 << int(core::largestPictureBytes + 17) << true << false;
        QTest::newRow("exact binary limit") << 2 << int(core::largestPictureBytes + 16) << false << true;
        QTest::newRow("fragmented exact binary limit") << 2 << int(core::largestPictureBytes + 16) << true << true;
        QTest::newRow("interrupted fragmented message") << 3 << 0 << false << false;
        QTest::newRow("fragmented ping") << 4 << 0 << false << false;
        QTest::newRow("masked server frame") << 5 << 0 << false << false;
        QTest::newRow("RSV bit") << 6 << 0 << false << false;
        QTest::newRow("64 bit length top bit") << 7 << 0 << false << false;
    }
    void liveFramesEnforceBoundsAndFraming()
    {
        QFETCH(int, kind);
        QFETCH(int, bytes);
        QFETCH(bool, fragments);
        QFETCH(bool, valid);
        const auto frame = [](quint8 first, const QByteArray& payload, bool masked = false) {
            QByteArray out(1, static_cast<char>(first));
            const quint64 n = static_cast<quint64>(payload.size());
            const quint8 mask = masked ? 0x80 : 0;
            if (n <= 125)
                out += static_cast<char>(mask | n);
            else if (n <= 65535) {
                out += static_cast<char>(mask | 126);
                out += char(n >> 8);
                out += char(n & 255);
            } else {
                out += static_cast<char>(mask | 127);
                for (int shift = 56; shift >= 0; shift -= 8)
                    out += char((n >> shift) & 255);
            }
            if (masked)
                out += QByteArray(4, '\0');
            return out + payload;
        };
        SilentServer server(SilentServer::Does::TakesTheLine);
        const Line line(server.address(), {}, LiveLink::Watch{1000, 60000, 60000});
        QTRY_COMPARE(line.opened->count(), 1);
        QByteArray wire;
        if (kind == 1 || kind == 2) {
            QByteArray data(bytes, ' ');
            if (kind == 1) {
                const QByteArray state("{\"t\":\"state\",\"state\":\"online\"}");
                data.replace(0, state.size(), state);
            } else {
                data.replace(0, 16, QByteArray(16, '\0'));
                data[0] = 2;
            }
            if (fragments) {
                const qsizetype split = data.size() / 2;
                wire = frame(quint8(kind), data.first(split)) + frame(0x89, "interleaved") +
                       frame(0x80, data.sliced(split));
            } else
                wire = frame(quint8(0x80 | kind), data);
        } else if (kind == 3)
            wire = frame(0x02, "partial") + frame(0x81, "{}");
        else if (kind == 4)
            wire = frame(0x09, "ping");
        else if (kind == 5)
            wire = QByteArray::fromHex("8182"); // Reject the mask bit before waiting for its payload.
        else if (kind == 6)
            wire = frame(0xC1, "{}");
        else
            wire = QByteArray::fromHex("827f8000000000000000");
        server.sendWire(wire);
        // (A picture at the limit is 32 MiB, as the server's are: on a loaded PC it takes seconds to cross the line.)
        const int pictureWithinMs = 20'000;
        if (valid) {
            if (kind == 1)
                QTRY_COMPARE_WITH_TIMEOUT(line.state->count(), 1, 3000);
            else {
                QTRY_COMPARE_WITH_TIMEOUT(line.pictures->count(), 1, pictureWithinMs);
                QCOMPARE(line.picture(0).data.size(), bytes - 16);
            }
            QCOMPARE(line.closed->count(), 0);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, kind == 2 ? pictureWithinMs : 1500);
            QCOMPARE(line.pictures->count(), 0);
        }
    }
    void keptAliveChallengesAndRedirectsAreRefused_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("header");
        QTest::newRow("401 NTLM keep-alive") << 401 << QByteArray("WWW-Authenticate: NTLM");
        QTest::newRow("401 Negotiate keep-alive") << 401 << QByteArray("WWW-Authenticate: Negotiate");
        QTest::newRow("407 NTLM keep-alive") << 407 << QByteArray("Proxy-Authenticate: NTLM");
        QTest::newRow("407 Negotiate keep-alive") << 407 << QByteArray("Proxy-Authenticate: Negotiate");
        QTest::newRow("302 keep-alive") << 302 << QByteArray("Location: /elsewhere");
    }
    void keptAliveChallengesAndRedirectsAreRefused()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, header);
        int requests = 0;
        bool authorization = false;
        StandIn server([&](const Asked& asked) {
            ++requests;
            authorization = authorization || asked.headers.contains("authorization") ||
                            asked.headers.contains("proxy-authorization");
            return Answered{status, {}, "text/plain", {header}};
        });
        server.handsLinesOver = false;
        server.keepsLines = true;
        const Line line(server.address(), {}, LiveLink::Watch{60000, 60000, 60000});
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 1500);
        QTest::qWait(100);
        QCOMPARE(requests, 1);
        QVERIFY(!authorization);
        QCOMPARE(line.opened->count(), 0);
    }
    void unknownLiveSchemesNeverContactTheServer_data()
    {
        QTest::addColumn<QString>("scheme");
        for (const auto& value : {u"ftp"_s, u"file"_s, u"http"_s, u"https"_s})
            QTest::newRow(qPrintable(value)) << value;
    }
    void unknownLiveSchemesNeverContactTheServer()
    {
        QFETCH(QString, scheme);
        LiveStandIn server;
        QUrl address(server.address());
        address.setScheme(scheme);
        address.setPath(u"/api/v1/devices/a/live"_s);
        LiveLink link(QNetworkRequest(address), pass, {}, {}, {});
        const QSignalSpy opened(&link, &LiveLink::opened);
        const QSignalSpy closed(&link, &LiveLink::closed);
        QTRY_VERIFY(opened.count() + closed.count() > 0);
        QCOMPARE(opened.count(), 0);
        QCOMPARE(server.lines, 0);
        QCOMPARE(closed.count(), 1);
    }
    void liveUpgradeRequiresItsHeadersAndNoUnrequestedOptions_data()
    {
        QTest::addColumn<QByteArray>("headers");
        QTest::addColumn<bool>("valid");
        QTest::newRow("no upgrade") << QByteArray("Connection: Upgrade\r\n") << false;
        QTest::newRow("no connection") << QByteArray("Upgrade: websocket\r\n") << false;
        QTest::newRow("wrong upgrade") << QByteArray("Upgrade: h2c\r\nConnection: Upgrade\r\n") << false;
        QTest::newRow("wrong token") << QByteArray("Upgrade: websocket\r\nConnection: NotUpgrade\r\n") << false;
        QTest::newRow("extension")
            << QByteArray(
                   "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Extensions: permessage-deflate\r\n")
            << false;
        QTest::newRow("subprotocol")
            << QByteArray("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: unrequested\r\n")
            << false;
        QTest::newRow("empty extension")
            << QByteArray("Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Extensions:\r\n") << false;
        QTest::newRow("mixed case token list")
            << QByteArray("uPgRaDe: WebSocket\r\nConnection: keep-alive, UpGrAdE\r\n") << true;
    }
    void liveUpgradeRequiresItsHeadersAndNoUnrequestedOptions()
    {
        QFETCH(QByteArray, headers);
        QFETCH(bool, valid);
        SilentServer server(SilentServer::Does::TakesTheLine);
        server.upgradeHeaders = headers;
        const Line line(server.address());
        QTRY_VERIFY(line.opened->count() + line.closed->count() > 0);
        QCOMPARE(line.opened->count(), valid ? 1 : 0);
        QCOMPARE(line.closed->count(), valid ? 0 : 1);
    }
    void theOneUseTicketIsWipedAfterUseOrAbandonment_data()
    {
        QTest::addColumn<int>("ending");
        QTest::newRow("hello sent") << 0;
        QTest::newRow("upgrade refused") << 1;
        QTest::newRow("closed before open") << 2;
    }
    void theOneUseTicketIsWipedAfterUseOrAbandonment()
    {
        QFETCH(int, ending);
        SilentServer server(ending == 1 ? SilentServer::Does::AnswersRubbish : SilentServer::Does::TakesTheLine);
        QUrl address(server.address());
        address.setScheme(u"ws"_s);
        address.setPath(u"/api/v1/devices/a/live"_s);
        QByteArray ticket(32, 'x');
        QVERIFY(ticket.isDetached());
        const QByteArrayView borrowed(ticket); // moved allocation remains owned by the live link until scope end
        LiveLink link(QNetworkRequest(address), std::move(ticket), {}, {}, {});
        const QSignalSpy opened(&link, &LiveLink::opened);
        const QSignalSpy closed(&link, &LiveLink::closed);
        if (ending == 2)
            link.close();
        else
            QTRY_VERIFY(opened.count() + closed.count() > 0);
        QVERIFY(std::ranges::all_of(borrowed, [](char c) { return c == '\0'; }));
    }
    void closedIsNeverSaidFromInsideCloseOrTheDestructor_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("close()") << false;
        QTest::newRow("destructor") << true;
    }
    void closedIsNeverSaidFromInsideCloseOrTheDestructor()
    {
        QFETCH(bool, destroy);
        SilentServer server(SilentServer::Does::TakesTheLine);
        // No pings (askEvery 60 s); silence and lateness after 1.5 s; room enough that nothing ends for size.
        Line line(server.address(), {}, LiveLink::Watch{5000, 60'000, 1500, qsizetype{16} * 1024 * 1024});
        QTRY_COMPARE(line.opened->count(), 1);
        const QByteArray grant = "{\"t\":\"control\",\"on\":true}";
        server.sendWire(QByteArray(1, '\x81') + QByteArray(1, static_cast<char>(grant.size())) + grant);
        QTRY_VERIFY(line.link->hasControl());
        // Pictures keep coming, so the silence watch never fires; the server stops reading.
        quint32 counter = 0;
        QTimer pictures;
        connect(&pictures, &QTimer::timeout, this, [&] { server.sendPicture(++counter); });
        pictures.start(100);
        server.stopReading();
        QElapsedTimer filling;
        filling.start();
        for (int said = 0; said < 150'000 && filling.elapsed() < 300 && line.closed->count() == 0; ++said) {
            line.link->askForAKeyFrame();
            if (said % 5000 == 4999)
                QCoreApplication::processEvents();
        }
        QCOMPARE(line.closed->count(), 0); // precondition: still open
        QTest::qWait(2000);                // what waits is now older than lateAfterMs (1.5 s)
        QCOMPARE(line.closed->count(), 0); // precondition: nothing ended it meanwhile
        const qsizetype controlBefore = line.control->count();
        if (destroy)
            line.link.reset();
        else
            line.link->close();
        QCOMPARE(line.closed->count(), 0);              // live_link.h:102
        QCOMPARE(line.control->count(), controlBefore); // nothing said from inside it either
    }

    void namedOwnershipAndTakeoverStayBoundToTheReportedHolder()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        line.link->askControl(true);
        QTRY_COMPARE(server.saidOf(u"control"_s).size(), 1);
        QVERIFY(!server.saidOf(u"control"_s).first().contains(u"take"_s));
        server.say({{u"t"_s, u"control"_s},
                    {u"on"_s, false},
                    {u"code"_s, u"taken"_s},
                    {u"by"_s, u"First operator"_s},
                    {u"from"_s, u"opaque-reference"_s}});
        QTRY_COMPARE(line.link->controlOwner().name, u"First operator"_s);
        QCOMPARE(line.link->controlOwner().reference, u"opaque-reference"_s);
        line.link->takeOver(u"First operator"_s);
        line.link->takeOver({});
        line.link->takeOver(u"opaque-reference"_s);
        QTRY_COMPARE(server.saidOf(u"control"_s).size(), 2);
        QCOMPARE(server.saidOf(u"control"_s).last(),
                 (QJsonObject{
                     {u"t"_s, u"control"_s}, {u"on"_s, true}, {u"take"_s, true}, {u"from"_s, u"opaque-reference"_s}}));
        server.say(
            {{u"t"_s, u"control"_s}, {u"on"_s, false}, {u"code"_s, u"taken"_s}, {u"by"_s, u"Second operator"_s}});
        QTRY_COMPARE(line.link->controlOwner().reference, u"Second operator"_s);
        line.link->takeOver(u"opaque-reference"_s);
        line.link->askControl(false);
        QTRY_COMPARE(server.saidOf(u"control"_s).size(), 3);
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, false}});
        QTest::qWait(30);
        // The controller acknowledges a refusal by giving back. That acknowledgement says
        // nothing about the other holder, and must not erase the takeover offer.
        QCOMPARE(line.link->controlOwner().reference, u"Second operator"_s);
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
        QTRY_VERIFY(line.link->controlOwner().name.isEmpty());
        server.say({{u"t"_s, u"control"_s},
                    {u"on"_s, false},
                    {u"code"_s, u"taken"_s},
                    {u"by"_s, u"Second operator"_s},
                    {u"from"_s, 42}});
        QTRY_COMPARE(line.link->controlOwner().name, u"Second operator"_s);
        QVERIFY(line.link->controlOwner().reference.isEmpty());
        line.link->close();
        QVERIFY(line.link->controlOwner().name.isEmpty());
    }

    void unsupportedMouseMessagesNeverLeaveTheClient()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.say({{u"t"_s, u"state"_s}, {u"state"_s, u"online"_s}, {u"pointer"_s, u"relative"_s}});
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
        QTRY_VERIFY(line.link->hasControl());
        line.link->pointerAt(100, 200);
        line.link->pointerBy(3, 4);
        QTRY_COMPARE(server.saidOf(u"mr"_s).size(), 1);
        QCOMPARE(server.saidOf(u"m"_s).size(), 0);
    }

    void losingTheConnectionClearsTheReportedOwner()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, false}, {u"code"_s, u"taken"_s}, {u"by"_s, u"Operator"_s}});
        QTRY_COMPARE(line.link->controlOwner().name, u"Operator"_s);
        server.hangUp();
        QTRY_VERIFY(!line.link->isOpen());
        QVERIFY(line.link->controlOwner().name.isEmpty());
    }

    void initTestCase()
    {
        // Windows' own TLS, as in the program, whichever test is run first: the client chooses it.
        const core::ServerClient first;
        qRegisterMetaType<LiveLink::Video>();
        qRegisterMetaType<LiveLink::Picture>();
    }

    void theServerIsAskedForAPassAsItIsAskedEverything()
    {
        StandIn server(
            [](const Asked&) { return Answered{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"}; });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        client.setSession("s1");
        QList<base::Result<QByteArray>> answers;
        client.askLive(u"a b/c"_s, [&](base::Result<QByteArray> answer) { answers.append(std::move(answer)); });
        QTRY_COMPARE(answers.size(), 1);
        QVERIFY(answers.at(0).ok());
        QCOMPARE(answers.at(0).value(), pass);
        QCOMPARE(server.asked.size(), 1);
        QCOMPARE(server.asked[0].method, QByteArray("POST"));
        // The machine's name cannot reach into the address, whatever is in it.
        QCOMPARE(server.asked[0].path, QByteArray("/api/v1/devices/a%20b%2Fc/live"));
        QCOMPARE(server.asked[0].headers.value("x-qkvm"), QByteArray("1"));
        QCOMPARE(server.asked[0].headers.value("cookie"), QByteArray("qkvm_session=s1"));
    }

    void aPassIsOnlyWhatCanBeOne_data()
    {
        QTest::addColumn<QByteArray>("answer");
        QTest::addColumn<QString>("code");
        QTest::newRow("another form of the live connection")
            << QByteArray(R"({"v": 3, "ticket": "p4ss-for-one-line_0123456789"})") << u"live_form"_s;
        QTest::newRow("no form named") << QByteArray(R"({"ticket": "p4ss-for-one-line_0123456789"})") << u"live_form"_s;
        QTest::newRow("no pass") << QByteArray(R"({"v": 2})") << u"bad_answer"_s;
        QTest::newRow("a pass too short to be one") << QByteArray(R"({"v": 2, "ticket": "abc"})") << u"bad_answer"_s;
        QTest::newRow("a pass with what no pass has in it")
            << QByteArray(R"({"v": 2, "ticket": "p4ss for one line\r\nX: 0123456789"})") << u"bad_answer"_s;
        QTest::newRow("a pass with a line's end after it")
            << QByteArray(R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789\n"})") << u"bad_answer"_s;
        QTest::newRow("a number") << QByteArray(R"({"v": 2, "ticket": 1234567890123456})") << u"bad_answer"_s;
        QTest::newRow("no object") << QByteArray("[]") << u"bad_answer"_s;
    }

    void aPassIsOnlyWhatCanBeOne()
    {
        QFETCH(QByteArray, answer);
        QFETCH(QString, code);
        const base::Result<QByteArray> read = core::livePassFrom(answer);
        QVERIFY(!read.ok());
        QCOMPARE(QString::fromStdString(read.error().code), code);
    }

    void aServerWithoutLivePicturesSaysSoInItsOwnWords()
    {
        StandIn server([](const Asked&) {
            return Answered{404, R"({"code": "no_live", "message": "This machine has no live picture"})"};
        });
        core::ServerClient client;
        QVERIFY(client.setAddress(server.address()).ok());
        QList<base::Result<QByteArray>> answers;
        client.askLive(u"a"_s, [&](base::Result<QByteArray> answer) { answers.append(std::move(answer)); });
        QTRY_COMPARE(answers.size(), 1);
        QVERIFY(!answers.at(0).ok());
        QCOMPARE(answers.at(0).error().code, std::string("no_live"));
        QCOMPARE(answers.at(0).error().message, std::string("This machine has no live picture"));
    }

    void theLineIsOpenedWithThePassAndSaysWhatItNeeds()
    {
        LiveStandIn server;
        const Line line(server.address(), {}, {}, u"a b/c"_s);
        QVERIFY(!line.link->isOpen());
        QTRY_COMPARE(line.opened->count(), 1);
        QVERIFY(line.link->isOpen());
        QCOMPARE(server.lines, 1);
        QCOMPARE(server.path, QByteArray("/api/v1/devices/a%20b%2Fc/live"));
        QCOMPARE(server.headers.value("x-qkvm"), QByteArray("1"));
        // The session does not go over this line: the pass is what it is opened with.
        QVERIFY(!server.headers.contains("cookie"));
        QTRY_COMPARE(server.said.size(), 1);
        QCOMPARE(server.said.at(0), (QJsonObject{{u"t"_s, u"hello"_s},
                                                 {u"v"_s, 2},
                                                 {u"ticket"_s, QString::fromLatin1(pass)},
                                                 {u"video"_s, QJsonArray{u"h264"_s, u"jpeg"_s}},
                                                 {u"max_w"_s, 1280},
                                                 {u"max_fps"_s, 20}}));
        QCOMPARE(line.closed->count(), 0);
    }

    void aMachineIsHadLiveAtTheAddressOfEveryOtherCall_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("in the clear") << false;
        QTest::newRow("over TLS, known by its certificate") << true;
    }

    void aMachineIsHadLiveAtTheAddressOfEveryOtherCall()
    {
        QFETCH(bool, encrypted);
        const Tls first = tlsOf("first");
        const auto answer = [](const Asked&) {
            return Answered{200, R"({"v": 2, "ticket": "p4ss-for-one-line_0123456789"})"};
        };
        const std::unique_ptr<StandIn> server =
            encrypted ? std::make_unique<StandIn>(answer, first) : std::make_unique<StandIn>(answer);
        core::ServerClient client;
        QVERIFY(client.setAddress(server->address()).ok());
        if (encrypted) {
            client.setKnownCertificate(sha256Of(first));
        }
        client.setSession("s1");
        QList<base::Result<QByteArray>> answers;
        client.askLive(u"a"_s, [&](base::Result<QByteArray> given) { answers.append(std::move(given)); });
        QTRY_COMPARE(answers.size(), 1);
        QVERIFY(answers.at(0).ok());

        const std::unique_ptr<LiveLink> link = client.openLive(u"a"_s, answers.at(0).value(), {1280, 20});
        const QSignalSpy opened(link.get(), &LiveLink::opened);
        const QSignalSpy pictures(link.get(), &LiveLink::pictureCame);
        QTRY_COMPARE(opened.count(), 1);
        QCOMPARE(server->live.lines, 1);
        QCOMPARE(server->live.path, QByteArray("/api/v1/devices/a/live"));
        QTRY_COMPARE(server->live.said.size(), 1);
        QCOMPARE(server->live.said.at(0)[u"ticket"_s].toString(), QString::fromLatin1(pass));
        // The call for the pass is among the server's calls, and the line is not.
        QCOMPARE(server->asked.size(), 1);
        server->live.send(2, true, 1, 0, "a JPEG");
        QTRY_COMPARE(pictures.count(), 1);
        // And the server goes on answering calls beside the line.
        client.askLive(u"a"_s, [&](base::Result<QByteArray> given) { answers.append(std::move(given)); });
        QTRY_COMPARE(answers.size(), 2);
        QVERIFY(link->isOpen());
    }

    void picturesAreHandedOverAsTheyCome()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.say({{u"t"_s, u"video"_s}, {u"codec"_s, u"h264"_s}, {u"w"_s, 1920}, {u"h"_s, 1080}});
        QTRY_COMPARE(line.video->count(), 1);
        QCOMPARE(line.video->at(0).at(0).value<LiveLink::Video>(), (LiveLink::Video{true, 1920, 1080}));
        server.send(1, true, 7, 1'000'000, "a key frame");
        server.send(1, false, 8, 1'033'000, "what builds on it");
        QTRY_COMPARE(line.pictures->count(), 2);
        LiveLink::Picture one = line.picture(0);
        QVERIFY(one.h264);
        QVERIFY(one.keyFrame);
        QCOMPARE(one.counter, 7U);
        QCOMPARE(one.takenUs, 1'000'000);
        QCOMPARE(one.data, QByteArray("a key frame"));
        one = line.picture(1);
        QVERIFY(!one.keyFrame);
        QCOMPARE(one.counter, 8U);
        QCOMPARE(one.takenUs, 1'033'000);
        QCOMPARE(one.data, QByteArray("what builds on it"));

        // A server's own screen comes as JPEGs, each a whole picture whatever it is flagged as.
        server.say({{u"t"_s, u"video"_s}, {u"codec"_s, u"jpeg"_s}, {u"w"_s, 800}, {u"h"_s, 600}});
        server.send(2, false, 9, 2'000'000, "a JPEG");
        QTRY_COMPARE(line.pictures->count(), 3);
        QCOMPARE(line.video->at(1).at(0).value<LiveLink::Video>(), (LiveLink::Video{false, 800, 600}));
        one = line.picture(2);
        QVERIFY(!one.h264);
        QVERIFY(one.keyFrame);
        QCOMPARE(one.data, QByteArray("a JPEG"));
        // And how the machine is, the moment it changes.
        server.say({{u"t"_s, u"state"_s},
                    {u"state"_s, u"no_signal"_s},
                    {u"error"_s, QJsonObject{{u"code"_s, u"no_signal"_s}, {u"message"_s, u"No signal"_s}}}});
        QTRY_COMPARE(line.state->count(), 1);
        QCOMPARE(line.state->at(0).at(0).toString(), u"no_signal"_s);
        QCOMPARE(line.state->at(0).at(1).toString(), u"no_signal"_s);
        QCOMPARE(line.state->at(0).at(2).toString(), u"No signal"_s);
        // A size no picture has is no size.
        server.say({{u"t"_s, u"video"_s}, {u"codec"_s, u"jpeg"_s}, {u"w"_s, -5}, {u"h"_s, 2'000'000'000}});
        QTRY_COMPARE(line.video->count(), 3);
        QCOMPARE(line.video->at(2).at(0).value<LiveLink::Video>(), (LiveLink::Video{false, 0, 0}));
    }

    void whatIsNoPictureAndNoWordIsNotTakenForOne()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        // Shorter than what stands before every picture; of a kind there is none of; nothing in
        // it; and words that are no words of this.
        server.sendRaw("short");
        server.sendRaw(QByteArray(16, '\0'));
        server.send(3, true, 1, 0, "of no kind");
        server.send(1, true, 2, 0, QByteArray());
        server.sayRaw(u"not JSON"_s);
        server.sayRaw(u"[1, 2]"_s);
        server.say({{u"t"_s, u"video"_s}, {u"codec"_s, u"vp9"_s}, {u"w"_s, 1}, {u"h"_s, 1}});
        server.say({{u"t"_s, u"what"_s}});
        // Then a picture: it comes, and nothing came before it.
        server.send(2, true, 3, 5, "a JPEG");
        QTRY_COMPARE(line.pictures->count(), 1);
        QCOMPARE(line.picture(0).data, QByteArray("a JPEG"));
        QCOMPARE(line.video->count(), 0);
        QCOMPARE(line.closed->count(), 0);
        QVERIFY(line.link->isOpen());
    }

    void aLineThatDoesNotBeginWithAKeyFrameShowsNothingUntilOneComes()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        // The server joined a stream in its middle: what comes first builds on what was never sent.
        server.send(1, false, 1, 0, "in the middle");
        server.send(1, false, 2, 0, "and on");
        QTRY_COMPARE(server.saidOf(u"key_frame"_s).size(), 1);
        QCOMPARE(line.pictures->count(), 0);
        server.send(1, true, 3, 0, "key");
        server.send(1, false, 4, 0, "fourth");
        QTRY_COMPARE(line.pictures->count(), 2);
        QCOMPARE(line.picture(0).data, QByteArray("key"));
        QCOMPARE(line.link->skipped(), 2U);
        QTest::qWait(100);
        QCOMPARE(server.saidOf(u"key_frame"_s).size(), 1);
        // A line of JPEGs has no such beginning: each is a picture by itself.
        LiveStandIn other;
        const Line still(other.address());
        QTRY_COMPARE(still.opened->count(), 1);
        other.send(2, false, 9, 0, "a JPEG");
        QTRY_COMPARE(still.pictures->count(), 1);
        QVERIFY(still.picture(0).keyFrame);
        QCOMPARE(other.saidOf(u"key_frame"_s).size(), 0);
        // H.264 that comes after it begins as a line does: with a key frame, or it is not shown.
        other.send(1, false, 10, 0, "builds on nothing");
        QTRY_COMPARE(other.saidOf(u"key_frame"_s).size(), 1);
        QCOMPARE(still.pictures->count(), 1);
        other.send(1, true, 11, 0, "key");
        other.send(1, false, 12, 0, "and on");
        QTRY_COMPARE(still.pictures->count(), 3);
        QCOMPARE(still.link->skipped(), 1U);
    }

    void afterAGapNothingIsHandedOverUntilAKeyFrame()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.send(1, true, 1, 0, "key");
        server.send(1, false, 2, 0, "second");
        // Three and four never came: five builds on what is not there. So does six.
        server.send(1, false, 5, 0, "fifth");
        server.send(1, false, 6, 0, "sixth");
        server.send(1, true, 7, 0, "key again");
        server.send(1, false, 8, 0, "eighth");
        QTRY_COMPARE(line.pictures->count(), 4);
        QCOMPARE(line.picture(0).data, QByteArray("key"));
        QCOMPARE(line.picture(1).data, QByteArray("second"));
        QCOMPARE(line.picture(2).data, QByteArray("key again"));
        QCOMPARE(line.picture(3).data, QByteArray("eighth"));
        QCOMPARE(line.link->skipped(), 2U);
        // A key frame was asked for, once for the gap.
        QTRY_COMPARE(server.saidOf(u"key_frame"_s).size(), 1);

        // The server brought the line forward itself, to a key frame: nothing is skipped, and
        // nothing asked.
        server.send(1, true, 20, 0, "brought forward");
        QTRY_COMPARE(line.pictures->count(), 5);
        QCOMPARE(line.link->skipped(), 2U);
        // JPEGs are whole pictures: a gap among them is a gap, and each that comes is shown.
        server.send(2, false, 30, 0, "a JPEG");
        QTRY_COMPARE(line.pictures->count(), 6);
        QTest::qWait(100);
        QCOMPARE(server.saidOf(u"key_frame"_s).size(), 1);

        // A second gap is asked about as the first was.
        server.send(1, true, 31, 0, "key the third");
        server.send(1, false, 33, 0, "after a second gap");
        QTRY_COMPARE(server.saidOf(u"key_frame"_s).size(), 2);
        QCOMPARE(line.pictures->count(), 7);
        QCOMPARE(line.link->skipped(), 3U);
    }

    void whoeverDecodesCanAskForAKeyFrame()
    {
        LiveStandIn server;
        const Line line(server.address());
        // (Before the line is open there is nobody to ask.)
        line.link->askForAKeyFrame();
        QTRY_COMPARE(line.opened->count(), 1);
        line.link->askForAKeyFrame();
        QTRY_COMPARE(server.saidOf(u"key_frame"_s).size(), 1);
        QCOMPARE(server.said.size(), 2);
    }

    void anotherNeedIsSaidAndTheSameIsNot()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        line.link->setNeed({1280, 20});
        line.link->setNeed({2560, 60});
        line.link->setNeed({2560, 60});
        QTRY_COMPARE(server.saidOf(u"shape"_s).size(), 1);
        QTest::qWait(100);
        QCOMPARE(server.saidOf(u"shape"_s),
                 (QList<QJsonObject>{{{u"t"_s, u"shape"_s}, {u"max_w"_s, 2560}, {u"max_fps"_s, 60}}}));
    }

    void keysAndTheMouseGoOutOnlyWhileTheLineHoldsThem()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.say({{u"t"_s, u"state"_s}, {u"state"_s, u"online"_s}, {u"pointer"_s, u"both"_s}});
        QTRY_COMPARE(line.link->pointerMode(), core::Live::Pointer::Both);
        // Not asked for yet: nothing of a key leaves the PC.
        line.link->key(0x04, true);
        line.link->pointerAt(100, 100);
        line.link->releaseAll();
        // Asked for: still nothing, until the server has said that the line holds them.
        line.link->askControl(true);
        line.link->key(0x04, true);
        QTRY_COMPARE(server.saidOf(u"control"_s).size(), 1);
        QCOMPARE(server.saidOf(u"control"_s).at(0), (QJsonObject{{u"t"_s, u"control"_s}, {u"on"_s, true}}));
        QVERIFY(!line.link->hasControl());
        QCOMPARE(server.said.size(), 2);

        server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
        QTRY_COMPARE(line.control->count(), 1);
        QCOMPARE(line.control->at(0).at(0).toBool(), true);
        QVERIFY(line.link->hasControl());
        // (Said twice by the server, it is said once here.)
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});

        line.link->key(0xe3, true); // the left Windows key
        line.link->key(0xe3, false);
        line.link->pointerAt(16000, 40000); // past the picture's edge: at its edge
        line.link->pointerBy(-3, 4);
        line.link->pointerBy(0, 0); // went nowhere
        line.link->button(1, true);
        line.link->button(1, false);
        line.link->button(6, true); // no such button
        line.link->wheel(0, -120);
        line.link->wheel(0, 0);
        line.link->key(0, true); // no such key
        line.link->releaseAll();
        QTRY_COMPARE(server.said.size(), 10);
        QTest::qWait(100);
        QCOMPARE(line.control->count(), 1);
        QCOMPARE(server.said.mid(2), (QList<QJsonObject>{{{u"t"_s, u"k"_s}, {u"u"_s, 0xe3}, {u"d"_s, true}},
                                                         {{u"t"_s, u"k"_s}, {u"u"_s, 0xe3}, {u"d"_s, false}},
                                                         {{u"t"_s, u"m"_s}, {u"x"_s, 16000}, {u"y"_s, 32767}},
                                                         {{u"t"_s, u"mr"_s}, {u"dx"_s, -3}, {u"dy"_s, 4}},
                                                         {{u"t"_s, u"mb"_s}, {u"b"_s, 1}, {u"d"_s, true}},
                                                         {{u"t"_s, u"mb"_s}, {u"b"_s, 1}, {u"d"_s, false}},
                                                         {{u"t"_s, u"mw"_s}, {u"dx"_s, 0}, {u"dy"_s, -120}},
                                                         {{u"t"_s, u"up"_s}}}));

        // The server takes them away (somebody else took them), and says why: from then on
        // nothing goes out again.
        server.say({{u"t"_s, u"control"_s},
                    {u"on"_s, false},
                    {u"code"_s, u"taken"_s},
                    {u"message"_s, u"anna has taken keyboard and mouse"_s}});
        QTRY_COMPARE(line.control->count(), 2);
        QCOMPARE(line.control->at(1).at(0).toBool(), false);
        QCOMPARE(line.control->at(1).at(1).toString(), u"taken"_s);
        QCOMPARE(line.control->at(1).at(2).toString(), u"anna has taken keyboard and mouse"_s);
        QVERIFY(!line.link->hasControl());
        line.link->key(0x04, true);
        QTest::qWait(100);
        QCOMPARE(server.said.size(), 10);
        // Asked for and not given: said too, with the server's reason.
        line.link->askControl(true);
        server.say({{u"t"_s, u"control"_s},
                    {u"on"_s, false},
                    {u"code"_s, u"forbidden"_s},
                    {u"message"_s, u"You may only watch"_s}});
        QTRY_COMPARE(line.control->count(), 3);
        QCOMPARE(line.control->at(2).at(1).toString(), u"forbidden"_s);
        // Given back from here.
        line.link->askControl(false);
        QTRY_COMPARE(server.saidOf(u"control"_s).size(), 3);
        QCOMPARE(server.saidOf(u"control"_s).at(2), (QJsonObject{{u"t"_s, u"control"_s}, {u"on"_s, false}}));
    }

    // Round 2 (review L2): a goodbye whose word is no server's to say (one with a ':', which is this
    // program's own kind of code, or one that is no text at all) ends the line for good, as a word this
    // program does not know does; a goodbye without a word is a passing end ("closed"). (Before: both
    // became "closed" and the line was opened again.)
    void aGoodbyeWhoseWordIsNoneOfTheServersEndsForGood_data()
    {
        QTest::addColumn<QJsonValue>("code"); // Undefined: no code at all
        QTest::addColumn<QString>("heard");
        QTest::addColumn<bool>("forGood");
        QTest::newRow("a word with a colon") << QJsonValue(u"policy:revoked"_s) << u"unrecognised"_s << true;
        QTest::newRow("a number") << QJsonValue(5) << u"unrecognised"_s << true;
        QTest::newRow("no word") << QJsonValue(QJsonValue::Undefined) << u"closed"_s << false;
        QTest::newRow("null, which says no word") << QJsonValue(QJsonValue::Null) << u"closed"_s << false;
        QTest::newRow("an empty word") << QJsonValue(QString()) << u"closed"_s << false;
        QTest::newRow("an object") << QJsonValue(QJsonObject{{u"a"_s, 1}}) << u"unrecognised"_s << true;
        QTest::newRow("true") << QJsonValue(true) << u"unrecognised"_s << true;
        QTest::newRow("a word it does not know") << QJsonValue(u"busy"_s) << u"busy"_s << true;
        QTest::newRow("a word of a passing end") << QJsonValue(u"slow"_s) << u"slow"_s << false;
    }
    void aGoodbyeWhoseWordIsNoneOfTheServersEndsForGood()
    {
        QFETCH(QJsonValue, code);
        QFETCH(QString, heard);
        QFETCH(bool, forGood);
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        QJsonObject bye{{u"t"_s, u"bye"_s}, {u"message"_s, u"said"_s}};
        if (!code.isUndefined())
            bye.insert(u"code"_s, code);
        server.say(bye);
        server.hangUp();
        QTRY_COMPARE(line.closed->count(), 1);
        QCOMPARE(line.closed->at(0).at(0).toString(), heard);
        QCOMPARE(line.link->endsForGood(heard), forGood);
    }
    void theServersLastWordIsWhyTheLineEnded()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
        QTRY_VERIFY(line.link->hasControl());
        server.hangUp(u"busy"_s, u"The machine's own console is in use by somebody at the machine"_s);
        QTRY_COMPARE(line.closed->count(), 1);
        QCOMPARE(line.closed->at(0).at(0).toString(), u"busy"_s);
        QCOMPARE(line.closed->at(0).at(1).toString(),
                 u"The machine's own console is in use by somebody at the machine"_s);
        QVERIFY(!line.link->isOpen());
        // Keyboard and mouse went with the line, and that is said before the line's end is.
        QVERIFY(!line.link->hasControl());
        QCOMPARE(line.control->count(), 2);
        QCOMPARE(line.control->at(1).at(0).toBool(), false);
        // Nothing is said twice, and nothing is sent into a line that is gone.
        line.link->key(0x04, true);
        line.link->setNeed({800, 5});
        QTest::qWait(150);
        QCOMPARE(line.closed->count(), 1);
    }

    void theServersLastWordIsHeardAlsoWhereItComesTogetherWithTheLinesEnd()
    {
        // The server says why and ends the line in the same breath, and the program does not look
        // for a moment: the word, the goodbye and the line's end are all with Windows when it
        // does. (Tried because the line is read by Qt's own WebSocket, which was taken to lose
        // what comes together with a line's end: where the line ends in order, as here, nothing
        // of it is lost, ten times of ten. A line that the far end resets is another matter, and
        // no reader's doing: device_line.h, at close().)
        for (int round = 0; round < 10; ++round) {
            LiveStandIn server;
            const Line line(server.address());
            QTRY_COMPARE(line.opened->count(), 1);
            server.say({{u"t"_s, u"state"_s}, {u"state"_s, u"online"_s}});
            server.hangUp(u"busy"_s, u"The machine's own console is in use by somebody at the machine"_s);
            QThread::msleep(100);
            QTRY_COMPARE(line.closed->count(), 1);
            QCOMPARE(line.closed->at(0).at(0).toString(), u"busy"_s);
            QCOMPARE(line.state->count(), 1);
        }
    }

    void whatMayStillBeDownWhenAServersLineFallsIsNotSaidHere()
    {
        // The server is the one to let go of what was down when its line falls, and has no word
        // for whether it could: a line to a server never says that something may still be down.
        LiveStandIn server;
        const Line line(server.address());
        const QSignalSpy leftDown(line.link.get(), &LiveLink::leftDown);
        QTRY_COMPARE(line.opened->count(), 1);
        line.link->askControl(true);
        server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
        QTRY_VERIFY(line.link->hasControl());
        line.link->key(0x04, true);
        QTRY_COMPARE(server.saidOf(u"k"_s).size(), 1);
        server.fallOver();
        QTRY_COMPARE(line.closed->count(), 1);
        QCOMPARE(line.control->count(), 2);
        QCOMPARE(line.control->at(1).at(0).toBool(), false);
        QTest::qWait(100);
        QCOMPARE(leftDown.count(), 0);
    }

    void aWordThatOnlyThisProgramSaysIsNotTakenFromAServer()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        // "The server could not be reached" is this program's own finding, and much hangs on it.
        server.say({{u"t"_s, u"control"_s},
                    {u"on"_s, false},
                    {u"code"_s, QString::fromLatin1(core::ServerClient::certificateChanged)},
                    {u"message"_s, u"so it says"_s}});
        QTRY_COMPARE(line.control->count(), 1);
        QCOMPARE(line.control->at(0).at(1).toString(), u"refused"_s);
        QCOMPARE(line.control->at(0).at(2).toString(), u"so it says"_s);
        server.hangUp(QString::fromLatin1(core::ServerClient::unreachable), u"so it says"_s);
        QTRY_COMPARE(line.closed->count(), 1);
        // (Still never this program's own word. Since round 2, L2, a goodbye that says one ends the line
        // for good, "unrecognised", where before it was a passing end, "closed".)
        QCOMPARE(line.closed->at(0).at(0).toString(), u"unrecognised"_s);
        QVERIFY(line.link->endsForGood(u"unrecognised"_s));
    }

    void aLineThatEndsWithoutAWordIsAServerThatIsGone()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        server.fallOver();
        QTRY_COMPARE(line.closed->count(), 1);
        QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QVERIFY(!line.link->isOpen());
    }

    void aServerThatIsNotThereIsSaidOnce()
    {
        const core::testing::NobodyThere nobody;
        const Line line(nobody.address());
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 10'000);
        QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QCOMPARE(line.opened->count(), 0);
        QTest::qWait(200);
        QCOMPARE(line.closed->count(), 1);
    }

    void aServerThatNeverTakesTheLineIsNotWaitedForForever()
    {
        const SilentServer server(SilentServer::Does::NeverAnswers);
        const Line line(server.address(), {}, LiveLink::Watch{300, 3000, 9000});
        QTRY_COMPARE(server.callers(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 3000);
        QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QCOMPARE(line.closed->at(0).at(1).toString(), u"qKVM did not answer for the live picture."_s);
        QCOMPARE(line.opened->count(), 0);
        // And the line is let go of: the server is not left with a caller that will say nothing.
        QTRY_COMPARE(server.callers(), 0);
    }

    void aLineThatCannotEvenBeBegunSaysSoToWhoeverListens()
    {
        // (No address a live line can go to: the library refuses it there and then, before
        // anybody can be listening for what the line says.)
        LiveLink link(QNetworkRequest(QUrl(u"http://127.0.0.1:9/api/v1/devices/a/live"_s)), pass, {}, {}, {});
        const QSignalSpy closed(&link, &LiveLink::closed);
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(closed.at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QTest::qWait(100);
        QCOMPARE(closed.count(), 1);
    }

    void aServerThatAnswersWithWhatIsNoAnswerIsLetGoOf()
    {
        const SilentServer server(SilentServer::Does::AnswersRubbish);
        const Line line(server.address());
        QTRY_COMPARE(line.closed->count(), 1);
        QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QCOMPARE(line.opened->count(), 0);
        QTRY_COMPARE(server.callers(), 0);
        QTest::qWait(100);
        QCOMPARE(line.closed->count(), 1);
    }

    void aServerThatFallsSilentIsNoticed()
    {
        // (A still screen sends no pictures: silence by itself says nothing. The server is asked
        // whether it is there, and this one never says so.)
        const SilentServer server(SilentServer::Does::TakesTheLine);
        const Line line(server.address(), {}, LiveLink::Watch{5000, 100, 400});
        QTRY_COMPARE(line.opened->count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 3000);
        QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QCOMPARE(line.closed->at(0).at(1).toString(), u"qKVM stopped answering on the live picture's line."_s);
        QTRY_COMPARE(server.callers(), 0);
    }

    void aServerOfWhichSomethingComesIsThereWhetherOrNotItAnswers_data()
    {
        QTest::addColumn<bool>("pictures");
        QTest::newRow("pictures") << true;
        QTest::newRow("words") << false;
    }

    void aServerOfWhichSomethingComesIsThereWhetherOrNotItAnswers()
    {
        // (The answer to "are you there" waits behind the pictures on a slow line: what comes
        // says as much.)
        QFETCH(bool, pictures);
        SilentServer server(SilentServer::Does::TakesTheLine);
        const Line line(server.address(), {}, LiveLink::Watch{5000, 100, 400});
        QTRY_COMPARE(line.opened->count(), 1);
        quint32 counter = 0;
        QTimer sending;
        connect(&sending, &QTimer::timeout, this, [&] {
            if (pictures) {
                server.sendPicture(++counter);
            } else {
                server.sendWord();
            }
        });
        sending.start(100);
        QTest::qWait(1300);
        QCOMPARE(line.closed->count(), 0);
        QVERIFY((pictures ? line.pictures->count() : line.state->count()) >= 5);
        // That stops as well: now it is gone.
        sending.stop();
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 3000);
    }

    void aServerThatTakesNothingMoreIsNotSentToWithoutEnd()
    {
        // It answers nothing and reads nothing, and keeps the line: what is said to it piles up
        // on this PC. Not without end. (The watch for silence is set too long to be what ends it.)
        SilentServer server(SilentServer::Does::TakesTheLine);
        const Line line(server.address(), {}, LiveLink::Watch{5000, 100, 60'000, 65'536});
        QTRY_COMPARE(line.opened->count(), 1);
        server.stopReading();
        // Far more than Windows keeps for a line that nobody reads.
        for (int said = 0; said < 150'000 && line.closed->count() == 0; ++said) {
            line.link->askForAKeyFrame();
            if (said % 5000 == 4999) {
                QCoreApplication::processEvents();
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 10'000);
        QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::unreachable));
        QCOMPARE(line.closed->at(0).at(1).toString(),
                 u"qKVM stopped taking what is sent on the live picture's line."_s);
    }

    void aServerThatAnswersIsNotTakenForGoneHoweverStillItsScreen()
    {
        LiveStandIn server;
        const Line line(server.address(), {}, LiveLink::Watch{5000, 100, 400});
        QTRY_COMPARE(line.opened->count(), 1);
        // No picture for well over the time a silent server is given: it answers when asked.
        QTest::qWait(1200);
        QCOMPARE(line.closed->count(), 0);
        QVERIFY(line.link->isOpen());
    }

    void aLineThatIsEndedFromHereSaysNothingMore()
    {
        LiveStandIn server;
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        QTRY_VERIFY(server.isConnected());
        line.link->close();
        QTRY_VERIFY(!server.isConnected());
        QTest::qWait(100);
        QCOMPARE(line.closed->count(), 0);
        QVERIFY(!line.link->isOpen());
    }

    void whatIsDownIsLetGoBeforeKeyboardAndMouseAreGivenBackOrTheLineIsEnded()
    {
        const auto kinds = [](const LiveStandIn& server) {
            QStringList found;
            for (const QJsonObject& word : server.said) {
                found.append(word[u"t"_s].toString());
            }
            return found;
        };
        // Given back: let go first, while the line still holds them.
        {
            LiveStandIn server;
            const Line line(server.address());
            QTRY_COMPARE(line.opened->count(), 1);
            server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
            QTRY_VERIFY(line.link->hasControl());
            line.link->key(0xe1, true); // the left Shift key, held
            line.link->askControl(false);
            QTRY_COMPARE(server.said.size(), 4);
            QCOMPARE(kinds(server), (QStringList{u"hello"_s, u"k"_s, u"up"_s, u"control"_s}));
        }
        // Ended from here.
        {
            LiveStandIn server;
            const Line line(server.address());
            QTRY_COMPARE(line.opened->count(), 1);
            server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
            QTRY_VERIFY(line.link->hasControl());
            line.link->key(0xe1, true);
            line.link->close();
            QTRY_VERIFY(!server.isConnected());
            QCOMPARE(kinds(server), (QStringList{u"hello"_s, u"k"_s, u"up"_s}));
        }
        // Thrown away while it holds them, by whoever had it: as much as can still be said is.
        {
            LiveStandIn server;
            Line line(server.address());
            QTRY_COMPARE(line.opened->count(), 1);
            server.say({{u"t"_s, u"control"_s}, {u"on"_s, true}});
            QTRY_VERIFY(line.link->hasControl());
            line.link->key(0xe1, true);
            line.link.reset();
            QTRY_VERIFY(!server.isConnected());
            QCOMPARE(kinds(server), (QStringList{u"hello"_s, u"k"_s, u"up"_s}));
        }
        // A line that never held them says nothing of the kind.
        {
            LiveStandIn server;
            const Line line(server.address());
            QTRY_COMPARE(line.opened->count(), 1);
            line.link->close();
            QTRY_VERIFY(!server.isConnected());
            QCOMPARE(kinds(server), (QStringList{u"hello"_s}));
        }
    }

    void aLineThatIsEndedBeforeItWasBegunIsNotBegun()
    {
        LiveStandIn server;
        const Line line(server.address());
        line.link->close();
        QTest::qWait(300);
        QCOMPARE(server.lines, 0);
        QCOMPARE(line.opened->count(), 0);
        QCOMPARE(line.closed->count(), 0);
    }

    void fragmentedVideoAndTextRemainBoundedAndControlFramesInterleave()
    {
        SilentServer server(SilentServer::Does::TakesTheLine);
        const Line line(server.address());
        QTRY_COMPARE(line.opened->count(), 1);
        const auto frame = [](char kind, const QByteArray& bytes) {
            return QByteArray(1, kind) + QByteArray(1, static_cast<char>(bytes.size())) + bytes;
        };
        QByteArray picture(16, '\0');
        picture[0] = 2;
        picture += "jpeg data";
        server.sendWire(frame('\x02', picture.first(8)) + frame('\x89', "ping") + frame('\x80', picture.sliced(8)));
        QTRY_COMPARE(line.pictures->count(), 1);
        QCOMPARE(line.picture(0).data, QByteArray("jpeg data"));
        server.sendWire(frame('\x01', R"({"t":"state",)") + frame('\x80', R"("state":"online"})"));
        QTRY_COMPARE(line.state->count(), 1);
        QCOMPARE(line.closed->count(), 0);
        // A continuation without an unfinished message is never a new picture.
        server.sendWire(frame('\x80', "orphan"));
        QTRY_COMPARE(line.closed->count(), 1);
        QCOMPARE(line.pictures->count(), 1);
    }

    void liveUpgradeNeverAnswersWindowsAuthentication_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("challenge");
        QTest::newRow("401 NTLM") << 401 << QByteArray("WWW-Authenticate: NTLM");
        QTest::newRow("401 Negotiate") << 401 << QByteArray("WWW-Authenticate: Negotiate");
        QTest::newRow("407 NTLM") << 407 << QByteArray("Proxy-Authenticate: NTLM");
        QTest::newRow("407 Negotiate") << 407 << QByteArray("Proxy-Authenticate: Negotiate");
    }
    void liveUpgradeNeverAnswersWindowsAuthentication()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, challenge);
        int handshakes = 0;
        bool authorization = false;
        StandIn server([&](const Asked& asked) {
            ++handshakes;
            // Do not print or retain any Windows authentication value in the evidence.
            authorization = authorization || asked.headers.contains("authorization") ||
                            asked.headers.contains("proxy-authorization");
            return Answered{status, {}, "text/plain", {challenge}};
        });
        server.handsLinesOver = false;
        LiveLink::Watch watch;
        watch.openWithinMs = 500;
        const Line line(server.address(), {}, watch);
        QTRY_COMPARE_WITH_TIMEOUT(line.closed->count(), 1, 3000);
        qInfo() << "handshakes" << handshakes << "authorization header present" << authorization;
        QTest::qWait(100);
        QCOMPARE(handshakes, 1);
        QVERIFY(!authorization);
        QCOMPARE(line.opened->count(), 0);
    }
    void theLiveLineGoesDirectlyEvenWhenAProxyIsConfigured()
    {
        const auto before = QNetworkProxy::applicationProxy();
        const auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(before); });
        int proxyCalls = 0;
        StandIn proxy([&](const Asked&) {
            ++proxyCalls;
            return Answered{407, {}, "text/plain", {"Proxy-Authenticate: NTLM"}};
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, u"127.0.0.1"_s, proxy.serverPort()));
        LiveStandIn server;
        LiveLink::Watch watch;
        watch.openWithinMs = 500;
        const Line line(server.address(), {}, watch);
        QTRY_VERIFY_WITH_TIMEOUT(line.opened->count() + line.closed->count() > 0, 3000);
        QCOMPARE(proxyCalls, 0);
        QCOMPARE(line.opened->count(), 1);
        QCOMPARE(line.closed->count(), 0);
    }

    void aServerThatWindowsVouchesForIsTakenUnlessItIsKnownByAnotherCertificate()
    {
        // (A certificate that a maker signed, and for this test the makers are trusted as the
        // ones Windows knows are.)
        const Tls shown = tlsOf("signed");
        const VouchedFor vouched(QStringLiteral(QKVM_TESTDATA "/tls/makers.crt"));
        // Known by no certificate: what Windows vouches for is taken.
        {
            LiveStandIn server(shown.certificateFile, shown.keyFile);
            const Line line(server.address());
            QTRY_COMPARE(line.opened->count(), 1);
            QTRY_COMPARE(server.said.size(), 1);
            QCOMPARE(line.closed->count(), 0);
        }
        // Known by another one: whoever else vouches for what it shows, the pass is not said to it.
        {
            LiveStandIn server(shown.certificateFile, shown.keyFile);
            const Line line(server.address(), sha256Of(tlsOf("first")));
            QTRY_COMPARE(line.closed->count(), 1);
            QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::certificateChanged));
            QCOMPARE(line.closed->at(0).at(1).toString(),
                     u"The server showed a certificate that is not the one it is known by."_s);
            QCOMPARE(line.opened->count(), 0);
            QTest::qWait(150);
            QCOMPARE(server.said.size(), 0);
            QCOMPARE(line.closed->count(), 1);
            // Even the upgrade is withheld: a trusted CA never overrides a pin.
            QCOMPARE(server.lines, 0);
        }
    }

    void aServerKnownByItsCertificateMayShowWhoSignedIt()
    {
        // A matching leaf may be accepted when every Schannel objection names it.
        // The empty-CA policy never relaxes the pin to a different leaf.
        const Tls shown = tlsOf("signed");
        LiveStandIn server(shown.certificateFile, shown.keyFile);
        const Line line(server.address(), sha256Of(shown));
        QTRY_COMPARE(line.opened->count(), 1);
        QTRY_COMPARE(server.said.size(), 1);
        QCOMPARE(line.closed->count(), 0);
    }

    void theLineIsOnlyOpenedToTheCertificateTheServerIsKnownBy()
    {
        const Tls first = tlsOf("first");
        const Tls second = tlsOf("second");
        // Known by the certificate it shows: opened, though Windows does not vouch for it.
        {
            LiveStandIn server(first.certificateFile, first.keyFile);
            const Line line(server.address(), sha256Of(first));
            QTRY_COMPARE(line.opened->count(), 1);
            QTRY_COMPARE(server.said.size(), 1);
            QCOMPARE(line.closed->count(), 0);
        }
        // Known by another one: not opened, and the pass is not said to it.
        {
            LiveStandIn server(first.certificateFile, first.keyFile);
            const Line line(server.address(), sha256Of(second));
            QTRY_COMPARE(line.closed->count(), 1);
            QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::certificateChanged));
            QCOMPARE(line.closed->at(0).at(1).toString(),
                     u"The server showed a certificate that is not the one it is known by."_s);
            QCOMPARE(line.opened->count(), 0);
            QTest::qWait(150);
            QCOMPARE(server.said.size(), 0);
            QCOMPARE(line.closed->count(), 1);
            // Not even the question for the line went to it.
            QCOMPARE(server.lines, 0);
        }
        // Known by none, and Windows does not vouch for it: not opened either.
        {
            LiveStandIn server(first.certificateFile, first.keyFile);
            const Line line(server.address());
            QTRY_COMPARE(line.closed->count(), 1);
            QCOMPARE(line.closed->at(0).at(0).toString(), QString::fromLatin1(core::ServerClient::certificateUnknown));
            QCOMPARE(line.closed->at(0).at(1).toString(),
                     u"The server showed a certificate that Windows does not trust."_s);
            QCOMPARE(line.opened->count(), 0);
            QTest::qWait(150);
            QCOMPARE(server.said.size(), 0);
            QCOMPARE(server.lines, 0);
        }
        // In the clear there is no certificate to show: a server that was once known by one is
        // spoken to as the calls speak to it.
        {
            LiveStandIn server;
            const Line line(server.address(), sha256Of(first));
            QTRY_COMPARE(line.opened->count(), 1);
            QTRY_COMPARE(server.said.size(), 1);
            QCOMPARE(line.closed->count(), 0);
        }
    }
};

QTEST_GUILESS_MAIN(TestLiveLink)

#include "test_live_link.moc"
