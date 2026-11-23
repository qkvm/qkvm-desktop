#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <qkvm/core/vnc_auth.h>

#include "rfb_by_hand.h"
#include "stand_in.h"

namespace qkvm::core::testing {

// OpenBMC's console by hand, as the web's stand-in has it (app/tests/fake_redfish.py's /kvm/0 with fake_vnc.py's
// VncState behind it): bmcweb's WebSocket at /kvm/0, let in with a Redfish session's token (X-Auth-Token) and no
// subprotocol, and behind it obmc-ikvm's RFB: version 3.8, no password, a screen of `width` by `height`.
//
// * Each request for an update is answered with the next of `updates` (whole messages of rfb_by_hand.h's; the last one
//   again where the requests outrun them), or, with none given, the whole of `picture` in Raw; a request for what
//   changed is answered only where `answersChanges` (a still screen answers nothing, except a request for all of it).
// * `pushEveryMs`: it sends its updates at its own pace as well, asked or not (as obmc-ikvm does).
// * What a session says comes in WebSocket messages of `piece` bytes (0: each message whole).
// * It takes the keys and the pointer it is sent (`keys`, `pointers`), and what else a client says (`requests`).
// * `security`, `greeting` and `silent`: what it offers, how it greets, and one that says nothing after the upgrade.
// * `hangsUpAfterMs`: it closes a session that long after the handshake; `garbles`: it answers a request with nothing
//   RFB has.
class RfbConsoleByHand : public QObject {
public:
    RfbConsoleByHand() : m_server(QStringLiteral("OpenBMC console stand-in"), QWebSocketServer::NonSecureMode)
    {
        stand = std::make_unique<StandIn>([](const Asked&) { return Answered(404, R"({"error":"no such thing"})"); });
        stand->handsLinesOver = false;
        stand->upgrade = [this](QTcpSocket* socket, const QByteArray& request) { return letIn(socket, request); };
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            while (QWebSocket* line = m_server.nextPendingConnection()) {
                serve(line);
            }
        });
        m_clock.start();
    }
    ~RfbConsoleByHand() override
    {
        for (const QPointer<QWebSocket>& line : m_lines) {
            delete line.data(); // NOLINT(cppcoreguidelines-owning-memory): the stand-in's, gone before it
        }
    }
    RfbConsoleByHand(const RfbConsoleByHand&) = delete;
    RfbConsoleByHand& operator=(const RfbConsoleByHand&) = delete;
    RfbConsoleByHand(RfbConsoleByHand&&) = delete;
    RfbConsoleByHand& operator=(RfbConsoleByHand&&) = delete;

    // What it is.
    QByteArray token = "t0ken"; // the session it lets in (where `admits` is not given)
    int width = 64;
    int height = 48;
    std::string name = "obmc-ikvm";
    rfb::tests::Picture picture = rfb::tests::pattern(64, 48, 1);
    std::vector<rfb::tests::Bytes> updates;
    bool answersChanges = true;
    bool answersWhole = true; // false (with answersChanges false): a server that answers no request at all
    int pushEveryMs = 0;
    int piece = 0;
    // With `piece`: the updates from this one on (by their place in `updates`) sent a piece every trickleEveryMs, as a
    // server that sends a message slowly.
    int trickleEveryMs = 0;
    std::size_t trickleFrom = 0;
    std::vector<std::uint8_t> security{1};
    // With VNC authentication among `security`: the password it takes (a door's ticket, as a Proxmox guest's console
    // has one); none: it answers no challenge, as obmc-ikvm does.
    std::optional<std::string> password;
    int passwordTries = 0; // answers to its challenge that came
    std::string greeting = "RFB 003.008\n";
    bool silent = false;
    int hangsUpAfterMs = 0;
    bool garbles = false;
    bool refuses = false; // answers the upgrade 403 (this account may not open the console)

    // What it was told and did.
    int askings = 0;  // the upgrade was asked for
    int sessions = 0; // ...and let in
    int watching = 0; // open now
    int mostWatching = 0;
    int clientClosed = 0; // sessions the client closed (its goodbye, or the line falling)
    // The longest its pushes stood still on a line that was not draining (more than 2 MiB waiting to go there):
    // the client holding its line back, seen from the server's side, from a little after the hold began.
    qint64 longestStallMs = 0;
    QList<QByteArray> tokens;
    QList<QByteArray> protocols; // the subprotocols asked for ("(none)")
    struct Request {
        bool incremental = false;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        qint64 atMs = 0;
    };
    std::vector<Request> requests;
    std::vector<std::pair<std::uint32_t, bool>> keys;
    struct Pointer {
        int x = 0;
        int y = 0;
        int buttons = 0;
        qint64 atMs = 0;
    };
    std::vector<Pointer> pointers;
    rfb::tests::Bytes handshake; // what the last session's client said before its first message
    qint64 initAtMs = -1;        // when the last session's handshake ended (its ServerInit said)
    std::vector<std::int32_t> encodings;
    int formats = 0;
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }
    qint64 sinceMs() const { return m_clock.elapsed(); }

    // A door of another kind (a Proxmox guest's vncwebsocket, c9b): given the whole asking, it says whether the asking
    // is this console's at all (else it is the stand-in's), and how it is answered: let in (no answer), or with what is
    // written in its place (empty: nothing at all, the line kept, as Proxmox answers a ticket used twice). With a door,
    // `tokens` notes the Authorization each asking carried.
    struct Door {
        bool ours = false;
        std::optional<QByteArray> answer;
    };
    std::function<Door(const QByteArray& request)> door;
    // Where an asking of another stand-in's comes to this console (a Proxmox's, which has a console for each guest).
    bool letInFrom(QTcpSocket* socket, const QByteArray& request) { return letIn(socket, request); }
    // The subprotocols its WebSocket answers with where one is asked for (Proxmox's "binary").
    void speaks(const QStringList& subprotocols) { m_server.setSupportedSubprotocols(subprotocols); }
    // With `silent`: the session is closed this long after the upgrade, nothing said on it (a guest's console whose
    // worker fails, or whose guest is stopped).
    int closesSilentlyAfterMs = 0;

    // Served at /kvm/0 of another stand-in as well (a controller's, which signs the sessions in), letting in the tokens
    // `admits` says. Everything else asked there is that stand-in's.
    void servesAt(StandIn& other, std::function<bool(const QByteArray&)> admits)
    {
        m_admits = std::move(admits);
        other.upgrade = [this](QTcpSocket* socket, const QByteArray& request) { return letIn(socket, request); };
    }

    // Every line open cut at once, with no goodbye said on it (a host gone from the network, a server's process
    // killed): its client sees the line fall, and no WebSocket close.
    void cutEveryLine()
    {
        for (const QPointer<QWebSocket>& line : m_lines) {
            if (line != nullptr) {
                line->abort();
            }
        }
    }

    // An update sent now, unasked, on every line open (a server whose screen just changed).
    void sendNow(const rfb::tests::Bytes& bytes)
    {
        for (const QPointer<QWebSocket>& line : m_lines) {
            if (line != nullptr) {
                send(line.data(), bytes);
            }
        }
    }

    // Requests that asked for the whole screen, and for what changed.
    int wholeRequests() const
    {
        return static_cast<int>(std::ranges::count_if(requests, [](const Request& one) { return !one.incremental; }));
    }

private:
    struct Seat {
        rfb::tests::Bytes heard;
        int stage = 0; // 0 its version, 1 its choice, 5 its answer to the challenge, 2 its init, 3 its messages
        std::array<std::uint8_t, vnc::challengeBytes> challenge{};
        std::size_t next = 0;
        qint64 stalledSince = -1; // its pushes standing still since (-1: they are not)
    };

    static std::optional<QByteArray> headerIn(const QByteArray& request, const QByteArray& name)
    {
        for (const QByteArray& line : request.split('\n')) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.first(colon).trimmed().toLower() == name) {
                return line.sliced(colon + 1).trimmed();
            }
        }
        return std::nullopt;
    }

    bool letIn(QTcpSocket* socket, const QByteArray& request)
    {
        if (door) {
            const Door said = door(request);
            if (!said.ours) {
                return false;
            }
            ++askings;
            tokens.append(headerIn(request, "authorization").value_or(QByteArray()));
            protocols.append(headerIn(request, "sec-websocket-protocol").value_or("(none)"));
            if (said.answer) {
                if (!said.answer->isEmpty()) {
                    socket->write(*said.answer);
                    socket->flush();
                }
                return true;
            }
            QTimer::singleShot(0, &m_server, [this, socket = QPointer<QTcpSocket>(socket)] {
                if (socket != nullptr) {
                    m_server.handleConnection(socket);
                    emit socket->readyRead();
                }
            });
            return true;
        }
        if (!request.startsWith("GET /kvm/0 ")) {
            return false;
        }
        ++askings;
        const QByteArray given = headerIn(request, "x-auth-token").value_or(QByteArray());
        tokens.append(given);
        protocols.append(headerIn(request, "sec-websocket-protocol").value_or("(none)"));
        if (refuses) {
            // "Not you": this account may not open the console (bmcweb answers 403 where its route's privileges fail).
            socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
            socket->flush();
            return true;
        }
        if (m_admits ? !m_admits(given) : given != token) {
            return false; // answered as any question: not here for whoever is not signed in
        }
        QTimer::singleShot(0, &m_server, [this, socket = QPointer<QTcpSocket>(socket)] {
            if (socket != nullptr) {
                m_server.handleConnection(socket);
                emit socket->readyRead();
            }
        });
        return true;
    }

    void send(QWebSocket* line, const rfb::tests::Bytes& bytes)
    {
        const QByteArray all(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
        if (piece <= 0) {
            line->sendBinaryMessage(all);
            return;
        }
        for (qsizetype at = 0; at < all.size(); at += piece) {
            line->sendBinaryMessage(all.mid(at, piece));
        }
    }

    // `bytes` a piece every trickleEveryMs (on the line's own timer, gone with it).
    void trickle(QWebSocket* line, const rfb::tests::Bytes& bytes)
    {
        const QByteArray all(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
        auto* pieces = new QTimer(line); // NOLINT(cppcoreguidelines-owning-memory): the line's
        auto at = std::make_shared<qsizetype>(0);
        // (This one's, as the pushes below are: a line may outlive this stand-in, its timer with it, as
        // jetkvm_by_hand.h's trickle found.)
        connect(pieces, &QTimer::timeout, this, [this, line, pieces, all, at] {
            if (*at >= all.size()) {
                pieces->stop();
                return;
            }
            line->sendBinaryMessage(all.mid(*at, piece));
            *at += piece;
        });
        pieces->start(trickleEveryMs);
    }

    rfb::tests::Bytes nextUpdate(Seat& seat)
    {
        if (garbles) {
            return {9, 9, 9};
        }
        if (updates.empty()) {
            return rfb::tests::update({rfb::tests::rawRect(0, 0, picture)});
        }
        const std::size_t at = std::min(seat.next, updates.size() - 1);
        ++seat.next;
        return updates[at];
    }

    void serve(QWebSocket* line)
    {
        ++sessions;
        mostWatching = std::max(mostWatching, ++watching);
        m_lines.append(line);
        auto seat = std::make_shared<Seat>();
        connect(line, &QWebSocket::disconnected, this, [this, line] {
            --watching;
            ++clientClosed;
            line->deleteLater();
        });
        if (silent) {
            if (closesSilentlyAfterMs > 0) {
                QTimer::singleShot(closesSilentlyAfterMs, line, [line] { line->close(); });
            }
            return;
        }
        send(line, rfb::tests::text(greeting));
        connect(line, &QWebSocket::binaryMessageReceived, this, [this, line, seat](const QByteArray& message) {
            seat->heard.insert(seat->heard.end(), message.begin(), message.end());
            take(line, *seat);
        });
        if (pushEveryMs > 0) {
            auto* pushes = new QTimer(line); // NOLINT(cppcoreguidelines-owning-memory): the line's
            connect(pushes, &QTimer::timeout, this, [this, line, seat] {
                // Pushed at its own pace, but not into a line that is not draining: a real encoder drops frames when
                // the client is slow (it does not buffer them without bound). Modelled by the bytes waiting to go.
                if (seat->stage != 3) {
                    return;
                }
                if (line->bytesToWrite() < qint64{2} * 1024 * 1024) {
                    seat->stalledSince = -1;
                    send(line, nextUpdate(*seat));
                    return;
                }
                if (seat->stalledSince < 0) {
                    seat->stalledSince = sinceMs();
                }
                longestStallMs = std::max(longestStallMs, sinceMs() - seat->stalledSince);
            });
            pushes->start(pushEveryMs);
        }
    }

    void take(QWebSocket* line, Seat& seat)
    {
        rfb::tests::Bytes& heard = seat.heard;
        for (;;) {
            if (seat.stage == 0) {
                if (heard.size() < 12) {
                    return;
                }
                handshake.assign(heard.begin(), heard.begin() + 12);
                heard.erase(heard.begin(), heard.begin() + 12);
                seat.stage = 1;
                send(line, rfb::tests::join({rfb::tests::Bytes{static_cast<std::uint8_t>(security.size())},
                                             rfb::tests::Bytes(security.begin(), security.end())}));
                continue;
            }
            if (seat.stage == 1) {
                if (heard.empty()) {
                    return;
                }
                const std::uint8_t chosen = heard.front();
                handshake.push_back(chosen);
                heard.erase(heard.begin());
                if (chosen == 2 && password) {
                    // VNC authentication: a challenge of its own for each session, answered as the program's own
                    // answer (vnc_auth.h, held to published vectors) makes it of `password`.
                    for (std::size_t at = 0; at < seat.challenge.size(); ++at) {
                        seat.challenge.at(at) =
                            static_cast<std::uint8_t>((static_cast<std::size_t>(sessions) * 29 + at * 11) & 0xFF);
                    }
                    send(line, rfb::tests::Bytes(seat.challenge.begin(), seat.challenge.end()));
                    seat.stage = 5;
                    continue;
                }
                seat.stage = 2;
                send(line, rfb::tests::word(0));
                continue;
            }
            if (seat.stage == 5) {
                if (heard.size() < seat.challenge.size()) {
                    return;
                }
                ++passwordTries;
                const rfb::tests::Bytes given(heard.begin(), heard.begin() + 16);
                heard.erase(heard.begin(), heard.begin() + 16);
                const std::string taken = password.value_or(std::string());
                const base::Result<vnc::Response> right =
                    vnc::answer(QByteArrayView(taken.data(), static_cast<qsizetype>(taken.size())), seat.challenge);
                if (!right.ok() || !std::equal(given.begin(), given.end(), right.value().begin())) {
                    send(line, rfb::tests::join({rfb::tests::word(1), rfb::tests::reason("Authentication failed")}));
                    return;
                }
                seat.stage = 2;
                send(line, rfb::tests::word(0));
                continue;
            }
            if (seat.stage == 2) {
                if (heard.empty()) {
                    return;
                }
                handshake.push_back(heard.front());
                heard.erase(heard.begin());
                seat.stage = 3;
                send(line, rfb::tests::serverInit(width, height, name));
                initAtMs = sinceMs();
                if (hangsUpAfterMs > 0) {
                    QTimer::singleShot(hangsUpAfterMs, line, [line] { line->close(); });
                }
                continue;
            }
            if (heard.empty()) {
                return;
            }
            const std::uint8_t kind = heard.front();
            std::size_t length = 0;
            switch (kind) {
            case 0:
                length = 20;
                break;
            case 2:
                length = heard.size() < 4 ? 4 : 4 + 4 * static_cast<std::size_t>((heard[2] << 8) | heard[3]);
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
            default:
                line->close(); // (nothing a client says)
                return;
            }
            if (heard.size() < length) {
                return;
            }
            const auto at16 = [&heard](std::size_t at) { return (heard[at] << 8) | heard[at + 1]; };
            if (kind == 0) {
                ++formats;
            } else if (kind == 2) {
                encodings.clear();
                for (std::size_t at = 4; at < length; at += 4) {
                    encodings.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(at16(at)) << 16 |
                                                                  static_cast<std::uint32_t>(at16(at + 2))));
                }
            } else if (kind == 3) {
                const Request asked{heard[1] != 0, at16(2), at16(4), at16(6), at16(8), sinceMs()};
                requests.push_back(asked);
                if (asked.incremental ? answersChanges : answersWhole) {
                    const std::size_t place = seat.next;
                    const rfb::tests::Bytes next = nextUpdate(seat);
                    if (trickleEveryMs > 0 && piece > 0 && place >= trickleFrom && !updates.empty()) {
                        trickle(line, next);
                    } else {
                        send(line, next);
                    }
                }
            } else if (kind == 4) {
                keys.emplace_back(static_cast<std::uint32_t>(at16(4)) << 16 | static_cast<std::uint32_t>(at16(6)),
                                  heard[1] != 0);
            } else {
                pointers.push_back({at16(2), at16(4), heard[1], sinceMs()});
            }
            heard.erase(heard.begin(), heard.begin() + static_cast<std::ptrdiff_t>(length));
        }
    }

    QWebSocketServer m_server;
    QList<QPointer<QWebSocket>> m_lines;
    std::function<bool(const QByteArray&)> m_admits;
    QElapsedTimer m_clock;
};

} // namespace qkvm::core::testing
