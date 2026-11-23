#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include "jetkvm_storage_by_hand.h"
#include "stand_in.h"

namespace qkvm::core::testing {

// A JetKVM by hand, as a real one answered: what it is asked over
// HTTP on a StandIn, and its signalling line, a WebSocket on the same port, on a WebSocket server of Qt's.
//
// * POST /auth/login-local with {"password": ...}: 200 and the cookie authToken, a new one each time, taken from
//   its making and its one session once the answer goes (the one before is no longer taken then, unless a test says
//   `keepsEverySession`); 401 for another password; 400 where it runs without one ("no password" refuses a sign-in).
// * GET /device/status: {"isSetup": ...} and nothing else, to anybody.
// * GET /device: {"deviceId": ...}, to whoever holds its session (anybody where it runs without a password).
// * /webrtc/signaling/client: let in with its session (anybody where it runs without a password), 401
//   otherwise; once in, its first word there is `device-metadata` with its version (its `otaState` is said
//   on a data channel, as the real one's). A JetKVM never set up lets nobody in (401) until it is.
// * POST /storage/upload?uploadId=, where it is given its storage (`storage`): to whoever holds its session.
class JetKvmByHand : public QObject {
public:
    // (At a port of the test's choosing where it is to take the place of another: StandIn.)
    explicit JetKvmByHand(const std::optional<Tls>& tls = {}, quint16 port = 0)
        : m_server(QStringLiteral("JetKVM stand-in"), QWebSocketServer::NonSecureMode)
    {
        const auto answer = [this](const Asked& asked) { return answered(asked); };
        stand = tls ? std::make_unique<StandIn>(answer, *tls, port) : std::make_unique<StandIn>(answer, port);
        // (A signalling line it refuses is answered 401 as any question, not handed to a server's live line.)
        stand->handsLinesOver = false;
        stand->upgrade = [this](QTcpSocket* socket, const QByteArray& request) {
            if (!request.startsWith("GET /webrtc/signaling/client ")) {
                return false;
            }
            ++lineAskings;
            lineCookies.append(cookieIn(request).value_or(QByteArrayLiteral("(none)")));
            if (refuseLines > 0 || !holdsTheSession(cookieIn(request).value_or(QByteArray()))) {
                refuseLines = std::max(0, refuseLines - 1);
                m_refusalAfterMs = lineRefusedAfterMs.isEmpty() ? 0 : lineRefusedAfterMs.takeFirst();
                return false; // answered as any question: 401 (answered)
            }
            if (linesSayNothing) {
                return true; // taken, and never answered
            }
            QTimer::singleShot(0, &m_server, [this, socket = QPointer<QTcpSocket>(socket)] {
                if (socket != nullptr) {
                    m_server.handleConnection(socket);
                    emit socket->readyRead();
                }
            });
            return true;
        };
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            QWebSocket* line = m_server.nextPendingConnection();
            ++linesIn;
            m_lines.append(line);
            connect(line, &QWebSocket::disconnected, line, &QObject::deleteLater);
            connect(line, &QWebSocket::textMessageReceived, this,
                    [this, line](const QString& text) { heardOnALine(line, text.toUtf8()); });
            if (saysAWordFirst) {
                line->sendTextMessage(QStringLiteral(R"({"type":"otaState","data":{"updating":false}})"));
            }
            if (saysItsVersion) {
                line->sendTextMessage(QString::fromUtf8(
                    QJsonDocument(
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("device-metadata")},
                                    {QStringLiteral("data"), QJsonObject{{QStringLiteral("deviceVersion"), version}}}})
                        .toJson(QJsonDocument::Compact)));
            }
        });
    }

    // What it is.
    QString password = QStringLiteral("secret"); // none: it runs without one
    bool isSetup = true;
    // Its own number: each stand-in's another, as two JetKVMs' are, unless a test makes two one device.
    QJsonValue deviceId = QStringLiteral("9a8b7c6d5e4f%1").arg(nextNumber(), 4, 16, QLatin1Char('0'));
    QString version = QStringLiteral("0.4.8");
    bool saysItsVersion = true;
    bool saysAWordFirst = false;  // another word on its signalling line before its version
    bool linesSayNothing = false; // its signalling line is taken and never answered
    int statusAfterMs = 0;        // its status answered only after this long
    int signInAfterMs = 0;        // its sign-ins answered only after this long, a yes and a no alike
    int switchesLateMs = 0;       // and the one before a yes ended so much later than the yes goes
    // What it answers a sign-in instead, where set (a 429 with this Retry-After, or a 500).
    std::optional<Answered> signInInstead;
    // So many askings for its signalling line are refused 401 though they hold its session.
    int refuseLines = 0;
    // What a refused asking for its signalling line is answered: 401, or 403 (the session may not have one).
    int lineRefusedWith = 401;
    // Its own number not handed out this once: GET /device answered 500.
    bool deviceFails = false;
    // The first GET /device asked under its session is answered only after this long, once: whoever asked it asks for
    // its signalling line that much later (a driver held up on a loaded PC).
    int firstNumberAfterMs = 0;
    // Every session it gave is taken from then on, not only the newest, as a kvmd's are: for a case of how the
    // sign-ins of several drivers are paced, where whether one of them asked under its session before the next one's
    // yes replaced it is a matter of the PC's load (a driver whose session was replaced signs in once more, by right).
    bool keepsEverySession = false;
    // An older one, which has no GET /device: answered 404, and gives no number.
    bool hasNoDevicePage = false;
    // Each asking for its signalling line that is refused, in turn, is answered so many milliseconds late.
    QList<int> lineRefusedAfterMs;
    // Its own image storage (jetkvm_storage_by_hand.h), which takes POST /storage/upload; none: it has no such route.
    // And how many uploads were open there each time GET /device was asked.
    JetKvmStorageByHand* storage = nullptr;
    QList<int> deviceAskedWithUploadsOpen;

    // What it was asked.
    int signIns = 0;
    // The least time between two sign-ins coming, by its own clock: one that goes only once the answer to the one
    // before was heard comes at least signInAfterMs after it.
    qint64 leastMsBetweenSignIns = std::numeric_limits<qint64>::max();
    QList<QByteArray> signInBodies;
    int lineAskings = 0;
    int linesIn = 0;
    QList<QByteArray> lineCookies; // the cookie each asking for its signalling line carried, or "(none)"
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }
    int asked(const QByteArray& path) const
    {
        int found = 0;
        for (const Asked& one : stand->asked) {
            found += one.path == path ? 1 : 0;
        }
        return found;
    }
    static int nextNumber()
    {
        static int next = 0;
        return ++next;
    }
    // Its session as it stands: somebody signs in at its own page, and the one before is no longer taken.
    void signInElsewhere() { m_session = "elsewhere-" + QByteArray::number(++m_sessions); }

    // A viewer's signalling on its line (M6 2f; fake_jetkvm.py:615-689). An offer, {"type":"offer","data":{"sd":
    // base64 of JSON {type, sdp}}}, is handed to `takesOffer`, which makes the peer and gives its answer bare
    // (empty: it says {"error": ...} instead); that goes back as {"type":"answer","data": base64 of JSON}, and its
    // addresses (`addresses`, as gathered by then) follow one by one, 10 ms apart, as `new-ice-candidate` with
    // sdpMid as `midSaid` has it, until none new came for 300 ms; then `moreAddresses`, in the same way.
    enum class Mid { Zero, Empty, None };
    std::function<QByteArray(const QByteArray& sdp)> takesOffer;
    std::function<QList<QByteArray>()> addresses;
    Mid midSaid = Mid::Zero;
    QList<QByteArray> moreAddresses;
    QList<QByteArray> firstAddresses; // said before its own, in the same way
    QByteArray saysInsteadOfAnAnswer; // said in the answer's place, where set
    bool answersNothing = false;      // an offer is heard and never answered
    bool closesLineAfterItsAddresses = false;
    bool saysItsAddressesFirst = false; // its addresses before its answer (gathered first, as pion may)
    QList<QByteArray> offersHeard;      // the description of each offer
    QList<QByteArray> offerMessages;    // each offer as it came
    int addressesSaid = 0;
    // Says a text on every line that is in.
    void sayOnLines(const QByteArray& text)
    {
        for (const QPointer<QWebSocket>& line : std::as_const(m_lines)) {
            if (line != nullptr) {
                line->sendTextMessage(QString::fromUtf8(text));
            }
        }
    }
    // Closes every line that is in, with its goodbye.
    void closeLines()
    {
        for (const QPointer<QWebSocket>& line : std::as_const(m_lines)) {
            if (line != nullptr) {
                line->close();
            }
        }
    }

private:
    void heardOnALine(QWebSocket* line, const QByteArray& text)
    {
        const QJsonObject message = QJsonDocument::fromJson(text).object();
        if (message.value(QStringLiteral("type")).toString() != QStringLiteral("offer")) {
            return;
        }
        offerMessages.append(text);
        const QByteArray packed =
            message.value(QStringLiteral("data")).toObject().value(QStringLiteral("sd")).toString().toLatin1();
        const QJsonObject offer = QJsonDocument::fromJson(QByteArray::fromBase64(packed)).object();
        const QByteArray sdp = offer.value(QStringLiteral("sdp")).toString().toUtf8();
        offersHeard.append(sdp);
        if (answersNothing) {
            return;
        }
        if (!saysInsteadOfAnAnswer.isEmpty()) {
            line->sendTextMessage(QString::fromUtf8(saysInsteadOfAnAnswer));
            return;
        }
        const QByteArray bare = takesOffer ? takesOffer(sdp) : QByteArray();
        if (bare.isEmpty()) {
            line->sendTextMessage(QStringLiteral(R"({"error":"no answer could be made"})"));
            return;
        }
        const QByteArray answer = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("answer")},
                                                            {QStringLiteral("sdp"), QString::fromUtf8(bare)}})
                                      .toJson(QJsonDocument::Compact)
                                      .toBase64();
        const auto sayTheAnswer = [line = QPointer<QWebSocket>(line), answer] {
            if (line != nullptr) {
                line->sendTextMessage(
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("answer")},
                                                                {QStringLiteral("data"), QString::fromLatin1(answer)}})
                                          .toJson(QJsonDocument::Compact)));
            }
        };
        if (saysItsAddressesFirst) {
            trickle(line, sayTheAnswer);
            return;
        }
        sayTheAnswer();
        trickle(line, {});
    }
    // Its addresses, then `then`.
    void trickle(QWebSocket* line, const std::function<void()>& then)
    {
        struct Trickling {
            QList<QByteArray> said;
            QList<QByteArray> first;
            QList<QByteArray> more;
            int quiet = 0;
        };
        auto state = std::make_shared<Trickling>();
        state->first = firstAddresses;
        state->more = moreAddresses;
        auto* every = new QTimer(line); // NOLINT(cppcoreguidelines-owning-memory): the line's, or its own once over
        every->setInterval(10);
        // (This one's: a line is no child of the server that took it (QWebSocketServer owns none), and may outlive this
        // stand-in by seconds, its timer with it; heard there, its turns read and wrote this one after it had gone,
        // and called what its `addresses` had been: test_core_jetkvm_live crashed so in a gate. And no turn after the
        // line: the timer is the line's.)
        connect(every, &QTimer::timeout, this, [this, line, every, state, then] {
            QByteArray next;
            const bool first = !state->first.isEmpty();
            if (first) {
                next = state->first.takeFirst(); // (said first, and not among its own)
            }
            for (const QByteArray& one : addresses && !first ? addresses() : QList<QByteArray>{}) {
                if (!state->said.contains(one)) {
                    next = one;
                    break;
                }
            }
            if (first) {
                state->quiet = 0;
            } else if (!next.isEmpty()) {
                state->quiet = 0;
                state->said.append(next);
            } else if (++state->quiet < (state->said.isEmpty() ? 300 : 30)) {
                return; // (its first one is waited for longer: gathering under a busy PC)
            } else if (!state->more.isEmpty()) {
                next = state->more.takeFirst();
            } else {
                every->stop();
                every->deleteLater();
                if (then) {
                    then();
                }
                if (closesLineAfterItsAddresses) {
                    line->close();
                }
                return;
            }
            QJsonObject data{{QStringLiteral("candidate"), QString::fromUtf8(next)},
                             {QStringLiteral("sdpMLineIndex"), 0}};
            if (midSaid != Mid::None) {
                data.insert(QStringLiteral("sdpMid"), midSaid == Mid::Zero ? QStringLiteral("0") : QString());
            }
            ++addressesSaid;
            line->sendTextMessage(QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("new-ice-candidate")},
                                          {QStringLiteral("data"), data}})
                    .toJson(QJsonDocument::Compact)));
        });
        every->start();
    }

    // The cookie an asking carried; none where it carried no Cookie at all.
    static std::optional<QByteArray> cookieIn(const QByteArray& request)
    {
        for (const QByteArray& line : request.split('\n')) {
            const qsizetype colon = line.indexOf(':');
            if (colon > 0 && line.first(colon).trimmed().toLower() == "cookie") {
                return line.sliced(colon + 1).trimmed();
            }
        }
        return std::nullopt;
    }
    bool holdsTheSession(const QByteArray& cookie) const
    {
        const auto carries = [&cookie](const QByteArray& session) {
            return !session.isEmpty() && cookie == "authToken=" + session;
        };
        return isSetup &&
               (password.isEmpty() || carries(m_session) || std::any_of(m_coming.begin(), m_coming.end(), carries) ||
                (keepsEverySession && std::any_of(m_given.begin(), m_given.end(), carries)));
    }
    void give(const QByteArray& session)
    {
        m_coming.removeOne(session);
        m_session = session;
        m_given.append(session);
    }

    Answered signIn(const Asked& asked)
    {
        if (signInInstead) {
            return *signInInstead;
        }
        if (!isSetup) {
            return {401, R"({"error":"Device is not set up"})"};
        }
        if (password.isEmpty()) {
            return {400, R"({"error":"Login is disabled in noPassword mode"})"};
        }
        if (QJsonDocument::fromJson(asked.body).object().value(QStringLiteral("password")).toString() != password) {
            return {401, R"({"error":"Invalid password"})"};
        }
        // Its session is taken from when it is made, before its answer goes, as a real one's is: nobody can carry it
        // sooner. The one before ends as the answer goes (a slow sign-in ends it only then), by a timer of this
        // stand-in's beside the answer's own. Qt does not keep two such timers in the order they were started: in a
        // whole run of test_core_jetkvm_device the answer's ran 31 ms before this one, and a driver that had heard its
        // yes was refused the session it carried, and signed in once more.
        const QByteArray made = "jet." + QByteArray::number(++m_sessions);
        m_coming.append(made);
        if (const int switchMs = signInAfterMs + switchesLateMs; switchMs > 0) {
            QTimer::singleShot(switchMs, this, [this, made] { give(made); });
        } else {
            give(made);
        }
        Answered in(200, R"({"message":"Login successful"})");
        in.more = {"Set-Cookie: authToken=" + made + "; Path=/; HttpOnly"};
        return in;
    }

    Answered answered(const Asked& asked)
    {
        if (asked.path == "/auth/login-local" && asked.method == "POST") {
            ++signIns;
            signInBodies.append(asked.body);
            const qint64 now = m_clock.elapsed();
            if (m_lastSignInAtMs >= 0) {
                leastMsBetweenSignIns = std::min(leastMsBetweenSignIns, now - m_lastSignInAtMs);
            }
            m_lastSignInAtMs = now;
            Answered said = signIn(asked);
            said.afterMs = std::max(said.afterMs, signInAfterMs);
            return said;
        }
        if (storage != nullptr && asked.method == "POST" && asked.path.startsWith("/storage/upload?")) {
            return storage->upload(asked, holdsTheSession(asked.headers.value("cookie")));
        }
        if (storage != nullptr && asked.path == "/device") {
            deviceAskedWithUploadsOpen.append(storage->openUploads());
        }
        if (asked.path == "/device/status") {
            Answered status(
                200, QJsonDocument(QJsonObject{{QStringLiteral("isSetup"), isSetup}}).toJson(QJsonDocument::Compact));
            status.afterMs = statusAfterMs;
            return status;
        }
        if (asked.path == "/device" && deviceFails) {
            return {500, R"({"error":"Internal Server Error"})"};
        }
        if (asked.path == "/device" && hasNoDevicePage) {
            return {404, R"({"error":"Not Found"})"};
        }
        if (asked.path == "/device" && holdsTheSession(asked.headers.value("cookie"))) {
            Answered number(
                200, QJsonDocument(QJsonObject{{QStringLiteral("deviceId"), deviceId}}).toJson(QJsonDocument::Compact));
            if (!password.isEmpty() && !std::exchange(m_numberHeldUp, true)) {
                number.afterMs = firstNumberAfterMs;
            }
            return number;
        }
        Answered refused(401, R"({"error":"Unauthorized"})");
        if (asked.path.startsWith("/webrtc/signaling/client")) {
            if (lineRefusedWith == 403) {
                refused = {403, R"({"error":"Forbidden"})"};
            }
            refused.afterMs = std::exchange(m_refusalAfterMs, 0);
        }
        return refused;
    }

    QWebSocketServer m_server;
    QByteArray m_session;
    QList<QByteArray> m_coming; // made, and taken, but not yet the one (the answer's way)
    QList<QByteArray> m_given;  // every session it gave (keepsEverySession)
    int m_sessions = 0;
    QElapsedTimer m_clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    qint64 m_lastSignInAtMs = -1; // leastMsBetweenSignIns
    bool m_numberHeldUp = false;  // firstNumberAfterMs was given
    int m_refusalAfterMs = 0;
    QList<QPointer<QWebSocket>> m_lines;
};

} // namespace qkvm::core::testing
