#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <qkvm/core/vnc_auth.h>

#include "rfb_by_hand.h"
#include "stand_in.h"

// A VNC server by hand, as the web's stand-in has it (app/tests/fake_vnc.py's FakeVnc and VncState): RFB straight on a
// TCP connection (`vnc://`), or over TLS from the first byte with a certificate a test names (`vncs://`), on the
// loopback address at a port Windows chooses.
//
// * Version 3.8 (or `greeting`), VNC authentication with `password` (none: security type None), checked against the
//   program's own answer (vnc_auth.h, itself held to published vectors); `offers` in place of what it offers.
// * Each request for an update is answered with the whole of `picture` in Raw where it asks for all of it, and where it
//   asks for what changed only while `answersChanges`; `sendNow` sends an update unasked.
// * It keeps what it is sent: the keys, the pointer, the requests, the password tries, how it was greeted.
// * Manners: `tooMany` (every sign-in answered "too many failures", 2), `endsAfterMs` (it ends every connection that
//   long after its ServerInit, as TigerVNC's MaxConnectionTime), `silent` (says nothing after the connection),
//   `garbles` (answers a request with what RFB has not), `garblesAfterChallenge` (the same after its challenge).
// * When each connection came and ended, by its own clock (sinceMs).
namespace qkvm::core::testing {

class VncByHand : public QObject {
public:
    explicit VncByHand(const std::optional<Tls>& tls = {}, quint16 port = 0) : m_encrypted(tls.has_value())
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
        m_server->listen(QHostAddress::LocalHost, port);
        connect(m_server.get(), &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (QTcpSocket* line = m_server->nextPendingConnection()) {
                serve(line);
            }
        });
        m_clock.start();
    }
    // The server goes first, and its callers with it.
    ~VncByHand() override
    {
        for (const QPointer<QTcpSocket>& line : m_lines) {
            if (line != nullptr) {
                line->abort();
            }
        }
        m_server.reset();
    }
    VncByHand(const VncByHand&) = delete;
    VncByHand& operator=(const VncByHand&) = delete;
    VncByHand(VncByHand&&) = delete;
    VncByHand& operator=(VncByHand&&) = delete;

    // What it is.
    std::optional<std::string> password = std::string("secret");
    std::optional<std::vector<std::uint8_t>> offers; // the security types it offers; none: as its password says
    std::string greeting = "RFB 003.008\n";
    int width = 160;
    int height = 100;
    std::string name = "stand-in VNC";
    rfb::tests::Picture picture = rfb::tests::pattern(160, 100, 1);
    bool answersChanges = true;
    bool tooMany = false;
    int endsAfterMs = 0;
    bool silent = false;
    bool garbles = false;
    bool garblesAfterChallenge = false; // its challenge, and in the same breath a result that means nothing (7)
    int hangsUpAfterKeys = 0;           // a connection ended at once once it was sent this many key events

    // What it was told and did.
    int connections = 0;
    std::vector<qint64> connectedAtMs; // (sinceMs) each connection's, and when each ended
    std::vector<qint64> endedAtMs;
    int handshakes = 0; // ServerInit said
    int passwordTries = 0;
    int rejected = 0;
    int open = 0;
    std::vector<std::string> versions; // what each client said it speaks
    std::vector<int> shared;           // ClientInit's flag, each time
    qint64 bytesHeard = 0;             // every byte any client sent
    struct Request {
        bool incremental = false;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<Request> requests;
    std::vector<std::pair<std::uint32_t, bool>> keys;
    std::vector<int> keysOn; // the connection (1 the first) each of `keys` came on
    struct Pointer {
        int x = 0;
        int y = 0;
        int buttons = 0;
    };
    std::vector<Pointer> pointers;

    quint16 port() const { return m_server->serverPort(); }
    // Its address as the program writes it: vnc:// (or vncs://) and the port, which is never 5900 here.
    QString url() const { return QStringLiteral("%1://127.0.0.1:%2").arg(m_encrypted ? u"vncs" : u"vnc").arg(port()); }
    qint64 sinceMs() const { return m_clock.elapsed(); }

    void sendNow(const rfb::tests::Bytes& bytes)
    {
        for (const QPointer<QTcpSocket>& line : m_lines) {
            if (line != nullptr) {
                send(line.data(), bytes);
            }
        }
    }
    // Every connection ended from here, at once.
    void hangUp()
    {
        for (const QPointer<QTcpSocket>& line : m_lines) {
            if (line != nullptr) {
                line->abort();
            }
        }
    }

    // What the keys it was sent leave down.
    std::vector<std::uint32_t> stillDown() const
    {
        std::vector<std::uint32_t> down;
        for (const auto& [keysym, pressed] : keys) {
            if (pressed) {
                down.push_back(keysym);
            } else {
                std::erase(down, keysym);
            }
        }
        return down;
    }

private:
    struct Seat {
        rfb::tests::Bytes heard;
        int stage = 0; // 0 its version, 1 its choice, 2 its answer to the challenge, 3 its init, 4 its messages
        std::array<std::uint8_t, core::vnc::challengeBytes> challenge{};
        int number = 0; // which connection (1 the first)
        int keys = 0;   // the key events it was sent
    };

    static void send(QTcpSocket* line, const rfb::tests::Bytes& bytes)
    {
        line->write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()));
    }

    std::vector<std::uint8_t> offered() const
    {
        if (offers) {
            return *offers;
        }
        return {static_cast<std::uint8_t>(password ? 2 : 1)};
    }

    void serve(QTcpSocket* line)
    {
        ++connections;
        ++open;
        connectedAtMs.push_back(sinceMs());
        m_lines.append(line);
        auto seat = std::make_shared<Seat>();
        seat->number = connections;
        connect(line, &QTcpSocket::disconnected, this, [this, line] {
            --open;
            endedAtMs.push_back(sinceMs());
            line->deleteLater();
        });
        if (silent) {
            return;
        }
        connect(line, &QTcpSocket::readyRead, this, [this, line, seat] {
            const QByteArray came = line->readAll();
            bytesHeard += came.size();
            seat->heard.insert(seat->heard.end(), came.begin(), came.end());
            take(line, *seat);
        });
        send(line, rfb::tests::text(greeting));
    }

    void take(QTcpSocket* line, Seat& seat)
    {
        rfb::tests::Bytes& heard = seat.heard;
        const auto drop = [&heard](std::size_t count) {
            heard.erase(heard.begin(), heard.begin() + static_cast<std::ptrdiff_t>(count));
        };
        for (;;) {
            if (seat.stage == 0) {
                if (heard.size() < 12) {
                    return;
                }
                versions.emplace_back(heard.begin(), heard.begin() + 12);
                drop(12);
                seat.stage = 1;
                const std::vector<std::uint8_t> types = offered();
                rfb::tests::Bytes list{static_cast<std::uint8_t>(types.size())};
                list.insert(list.end(), types.begin(), types.end());
                send(line, list);
                continue;
            }
            if (seat.stage == 1) {
                if (heard.empty()) {
                    return;
                }
                const std::uint8_t chosen = heard.front();
                drop(1);
                if (chosen == 2 && password) {
                    for (std::size_t at = 0; at < seat.challenge.size(); ++at) {
                        seat.challenge.at(at) =
                            static_cast<std::uint8_t>((static_cast<std::size_t>(connections) * 31 + at * 7) & 0xFF);
                    }
                    rfb::tests::Bytes asked(seat.challenge.begin(), seat.challenge.end());
                    if (garblesAfterChallenge) {
                        asked = rfb::tests::join({asked, rfb::tests::word(7)});
                    }
                    send(line, asked);
                    seat.stage = 2;
                    continue;
                }
                if (tooMany) {
                    send(line, rfb::tests::join({rfb::tests::word(2)}));
                    line->disconnectFromHost();
                    return;
                }
                send(line, rfb::tests::word(0));
                seat.stage = 3;
                continue;
            }
            if (seat.stage == 2) {
                if (heard.size() < core::vnc::challengeBytes) {
                    return;
                }
                ++passwordTries;
                const rfb::tests::Bytes given(heard.begin(), heard.begin() + core::vnc::challengeBytes);
                drop(core::vnc::challengeBytes);
                const std::string taken = password.value_or(std::string());
                const base::Result<core::vnc::Response> right = core::vnc::answer(
                    QByteArrayView(taken.data(), static_cast<qsizetype>(taken.size())), seat.challenge);
                if (tooMany) {
                    send(line, rfb::tests::word(2));
                    line->disconnectFromHost();
                    return;
                }
                if (!right.ok() || !std::equal(given.begin(), given.end(), right.value().begin())) {
                    ++rejected;
                    send(line, rfb::tests::join({rfb::tests::word(1), rfb::tests::reason("Authentication failed")}));
                    line->disconnectFromHost();
                    return;
                }
                send(line, rfb::tests::word(0));
                seat.stage = 3;
                continue;
            }
            if (seat.stage == 3) {
                if (heard.empty()) {
                    return;
                }
                shared.push_back(heard.front());
                drop(1);
                seat.stage = 4;
                ++handshakes;
                send(line, rfb::tests::serverInit(width, height, name));
                if (endsAfterMs > 0) {
                    QTimer::singleShot(endsAfterMs, line, [line] { line->disconnectFromHost(); });
                }
                continue;
            }
            if (heard.empty()) {
                return;
            }
            std::size_t length = 0;
            switch (heard.front()) {
            case 0: // SetPixelFormat
                length = 20;
                break;
            case 2: // SetEncodings
                if (heard.size() < 4) {
                    return;
                }
                length = 4 + std::size_t{4} * ((std::size_t{heard[2]} << 8) | heard[3]);
                break;
            case 3:
                length = 10;
                break;
            case 4:
                length = 8;
                break;
            case 5:
                length = 6;
                break;
            case 6:
                if (heard.size() < 8) {
                    return;
                }
                length = 8 + ((std::size_t{heard[4]} << 24) | (std::size_t{heard[5]} << 16) |
                              (std::size_t{heard[6]} << 8) | heard[7]);
                break;
            default:
                line->abort(); // what no client says
                return;
            }
            if (heard.size() < length) {
                return;
            }
            const rfb::tests::Bytes message(heard.begin(), heard.begin() + static_cast<std::ptrdiff_t>(length));
            drop(length);
            const auto be16 = [&message](std::size_t at) { return (int{message[at]} << 8) | message[at + 1]; };
            if (message[0] == 3) {
                Request asked{message[1] != 0, be16(2), be16(4), be16(6), be16(8)};
                requests.push_back(asked);
                if (garbles) {
                    send(line, {9, 9, 9});
                } else if (!asked.incremental || answersChanges) {
                    send(line, rfb::tests::update({rfb::tests::rawRect(0, 0, picture)}));
                }
            } else if (message[0] == 4) {
                const std::uint32_t keysym = (std::uint32_t{message[4]} << 24) | (std::uint32_t{message[5]} << 16) |
                                             (std::uint32_t{message[6]} << 8) | message[7];
                keys.emplace_back(keysym, message[1] != 0);
                keysOn.push_back(seat.number);
                if (hangsUpAfterKeys > 0 && ++seat.keys >= hangsUpAfterKeys) {
                    line->abort();
                    return;
                }
            } else if (message[0] == 5) {
                pointers.push_back({be16(2), be16(4), message[1]});
            }
        }
    }

    bool m_encrypted;
    std::unique_ptr<QTcpServer> m_server;
    QList<QPointer<QTcpSocket>> m_lines;
    QElapsedTimer m_clock;
};

// A vncs:// server by hand at a port where every line is told by its caller's first byte, and by nothing else: one that
// begins a TLS record (0x16, as TLS's hello does) is handed byte for byte to `behind`, a VncByHand over TLS with the
// certificate a test names; one whose first byte is any other, or that ends with none, came in the clear. No timer
// decides, and nothing is greeted in the clear: a caller in the clear waits there for a greeting that never comes, and
// is counted as soon as it speaks or ends. Every line is counted as it is accepted, one that ends at once among them;
// `undecided()` are those that have done neither yet (outside review c9a-3, L3: a 400 ms timer on the test's own
// event loop decided before, and a line in the clear that ended within it was never counted).
class VncEitherWay : public QObject {
public:
    explicit VncEitherWay(const Tls& tls) : behind(tls)
    {
        m_front.listen(QHostAddress::LocalHost);
        connect(&m_front, &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (QTcpSocket* line = m_front.nextPendingConnection()) {
                take(line);
            }
        });
    }
    ~VncEitherWay() override
    {
        for (const QPointer<QTcpSocket>& line : m_lines) {
            if (line != nullptr) {
                line->abort();
            }
        }
    }
    VncEitherWay(const VncEitherWay&) = delete;
    VncEitherWay& operator=(const VncEitherWay&) = delete;
    VncEitherWay(VncEitherWay&&) = delete;
    VncEitherWay& operator=(VncEitherWay&&) = delete;

    VncByHand behind;
    // Every line accepted; those whose first byte began a TLS record; those that came in the clear, and what their
    // callers sent on them.
    int connections = 0;
    int overTls = 0;
    int inTheClear = 0;
    qint64 clearBytesHeard = 0;
    int undecided() const { return connections - overTls - inTheClear; }

    quint16 port() const { return m_front.serverPort(); }
    QString url() const { return QStringLiteral("vncs://127.0.0.1:%1").arg(port()); }

private:
    // The first byte of every TLS record of a handshake, a ClientHello's among them.
    static constexpr char handshakeRecord = 0x16;

    void take(QTcpSocket* line)
    {
        ++connections;
        m_lines.append(line);
        auto decided = std::make_shared<bool>(false);
        auto first = std::make_shared<QMetaObject::Connection>();
        *first = connect(line, &QTcpSocket::readyRead, this, [this, line, decided, first] {
            disconnect(*first);
            *decided = true;
            char begins = 0;
            if (line->peek(&begins, 1) == 1 && begins == handshakeRecord) {
                ++overTls;
                pipe(line);
                return;
            }
            ++inTheClear;
            clearBytesHeard += line->readAll().size();
            connect(line, &QTcpSocket::readyRead, this, [this, line] { clearBytesHeard += line->readAll().size(); });
        });
        connect(line, &QTcpSocket::disconnected, this, [line, decided, this] {
            if (!std::exchange(*decided, true)) {
                ++inTheClear;
            }
            line->deleteLater();
        });
    }

    void pipe(QTcpSocket* line)
    {
        // (The caller's line's, and goes with it.)
        auto* back = new QTcpSocket(line); // NOLINT(cppcoreguidelines-owning-memory)
        auto held = std::make_shared<QByteArray>(line->readAll());
        connect(line, &QTcpSocket::readyRead, back, [line, back, held] {
            if (back->state() == QAbstractSocket::ConnectedState) {
                back->write(line->readAll());
            } else {
                held->append(line->readAll());
            }
        });
        connect(back, &QTcpSocket::connected, back, [back, held] {
            back->write(*held);
            held->clear();
        });
        connect(back, &QTcpSocket::readyRead, line, [line, back] { line->write(back->readAll()); });
        connect(back, &QTcpSocket::disconnected, line, [line] { line->disconnectFromHost(); });
        connect(line, &QTcpSocket::disconnected, back, [back] { back->abort(); });
        back->connectToHost(QHostAddress::LocalHost, behind.port());
    }

    QTcpServer m_front;
    QList<QPointer<QTcpSocket>> m_lines;
};

} // namespace qkvm::core::testing
