#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <qkvm/bmc/ami_console.h>

#include "aspeed_frames.h"
#include "stand_in.h"

namespace qkvm::core::testing {

// A management controller's console by hand (AMI's firmware), as the web's stand-in has it (app/tests/fake_redfish.py,
// console_stream), which encodes what two real boards said: a WebSocket at /kvm on a StandIn, on a WebSocket server
// of Qt's, let in with the session cookie of its web page and the subprotocol "binary".
//
// * It says it is allowed (kind::allowed); answered with a pass it made out (`passes`, each good once), it says who
//   else is there (`heldBy`, `shutOut`, `holderReturning`), takes the pass, asks about its licence, says who else has
//   it open, and, where nobody else is there, "the console is yours".
// * "Send the picture" (or "the whole screen") has the screen sent, frame after frame (`screen`), each in pieces of
//   `piece` bytes; once, as a console sends what changes after that.
// * With nobody else to hear it asks "still there?" every so often, puts the questions a test gives it (`asks`) and
//   what a test has it say (`says`), and ends a session after `sessionLastsMs`, its time being up.
// * `seats` at once, and a newcomer beyond them is told so; firmware that `reconnects` wants "the connection is
//   complete" before the pass, and closes without a word on a console that begins otherwise.
// * Keys and the mouse it is sent are decoded as the controller checks them (`hid`, and what does not check out in
//   `badHid`); in a `firstSession` a report that comes sooner than `hidSettlesMs` after the pass is lost
//   (`hidLost`), as on a real board, whose keyboard is attached only then. It can stop taking anything off its
//   line (`stall`) and take it again (`readAgain`): what is said to it then waits, with Windows and then with
//   whoever said it.
class AmiConsoleByHand : public QObject {
public:
    explicit AmiConsoleByHand(const std::optional<Tls>& tls = {})
        : m_server(QStringLiteral("AMI console stand-in"), QWebSocketServer::NonSecureMode)
    {
        const auto answer = [](const Asked& asked) {
            return asked.path.startsWith("/kvm") ? Answered(401, R"({"error":"sign in first"})")
                                                 : Answered(404, R"({"error":"no such thing"})");
        };
        stand = tls ? std::make_unique<StandIn>(answer, *tls) : std::make_unique<StandIn>(answer);
        stand->handsLinesOver = false;
        m_server.setSupportedSubprotocols({QStringLiteral("binary")});
        stand->upgrade = [this](QTcpSocket* socket, const QByteArray& request) { return letIn(socket, request); };
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            while (QWebSocket* line = m_server.nextPendingConnection()) {
                serve(line);
            }
        });
    }
    // Its lines go first, and each seat with its line: a seat's timer reads the stand-in, also after a stall.
    ~AmiConsoleByHand() override
    {
        for (const QPointer<QTcpSocket>& under : m_handed) {
            if (under != nullptr) {
                under->blockSignals(false);
            }
        }
        for (const auto& line : m_lines) {
            delete line.first.data(); // NOLINT(cppcoreguidelines-owning-memory): the stand-in's, gone before it
        }
    }
    AmiConsoleByHand(const AmiConsoleByHand&) = delete;
    AmiConsoleByHand& operator=(const AmiConsoleByHand&) = delete;
    AmiConsoleByHand(AmiConsoleByHand&&) = delete;
    AmiConsoleByHand& operator=(AmiConsoleByHand&&) = delete;

    // What it is.
    QByteArray session = "s3ss10n";                 // its web page's session, the cookie a console is let in with
    QSet<QByteArray> passes = {"a-console-pass"};   // the console passes it made out, each good for one session
    std::vector<bmc::aspeed::frames::Frame> screen; // what is on the server's screen, frame after frame
    int piece = 50;                                 // a frame is sent in pieces of this many bytes
    // The frames of `screen` this far apart, the first at once; 0: all in one breath. (Frames that come in one breath
    // may be read before the screen is shown, and are then shown as one picture.)
    int frameEveryMs = 0;
    // Where it is set, each frame of `screen` after the first is sent only once this says so of its place there, looked
    // at every 10 ms (and frameEveryMs is not used): a test that looks at frames shown one by one says when the one
    // before was shown, and does not stand on the reader beating the interface thread's next turn.
    std::function<bool(std::size_t)> sendsFrameWhen;
    bool beginsWithNothing = false; // to a console opened again: "nothing changed" first, the screen when asked
    int seats = 2;
    int sessionLastsMs = 0;       // it ends a session after this long, its time being up; 0: never
    QByteArray heldBy;            // somebody had the console first: whoever comes is told to wait
    bool shutOut = false;         // ...or that nobody else comes in
    bool holderReturning = false; // the one whose console dropped may come back (and the picture is sent)
    bool reconnects = false;      // wants "the connection is complete" first
    bool silent = false;          // takes the asking for the console and never answers
    bool deaf = false;            // takes nothing more off its line (not even a goodbye), and says nothing more either
    bool mute = false;            // says nothing more, not even "still there?" (set while a console is open)
    bool hangsUp = false;         // closes the console right after what it was given to say (`says`)
    bool deafAfterSaying = false; // ...or goes deaf right after it, before it hears a word of the answer
    int stillThereEveryMs = 300;
    std::vector<std::pair<std::uint16_t, QByteArray>> asks; // somebody asks for the console: kind, who
    std::vector<bmc::ami::Bytes> says;                      // what it says next, whole messages
    std::vector<bmc::ami::Bytes> saysFirst; // what it says right after "allowed", before the pass is answered
    bool firstSession = false;              // every session is a first one (the pass is answered with 2)...
    int hidSettlesMs = 0;                   // ...and its keyboard and mouse are attached this long after the pass
    std::uint8_t mouseMode = bmc::ami::mouse::absolute; // which mouse it says the server shows

    // What it was told and did.
    struct Heard {
        std::uint16_t kind;
        std::uint16_t status;
        qsizetype bytes;
    };
    std::vector<Heard> heard;
    struct Answer {
        std::uint16_t kind;
        std::uint16_t status;
        QByteArray who;
        qint64 afterMs;
    };
    std::vector<Answer> answers;
    // A keyboard or mouse report, decoded: a keyboard's modifiers and keys down, or a mouse's buttons, place and wheel
    // (for a relative one, how far it moved); its number, and when it came (milliseconds since the stand-in was made).
    struct Hid {
        bool keyboard = true;
        bool relative = false;
        int bits = 0; // the modifiers, or the buttons
        std::vector<int> keys;
        int x = 0;
        int y = 0;
        int wheel = 0;
        std::uint32_t number = 0;
        qint64 atMs = 0;
    };
    std::vector<Hid> hid;
    QList<QByteArray> badHid; // why a report did not check out
    int hidLost = 0;          // reports that came before the keyboard and mouse were attached
    // Sessions that ended with a key or a button down, by their last reports: a real board lets go of it then.
    int endedWithSomethingDown = 0;
    int askings = 0;      // how often the console was asked for
    int sessions = 0;     // ...and let in
    int watching = 0;     // open now
    int mostWatching = 0; // ...and the most that were open at once
    QList<QByteArray> origins;
    std::unique_ptr<StandIn> stand;

    QUrl address() const { return QUrl(stand->address()); }

    // Its console at /kvm of another stand-in as well (a controller's, whose own web page hands out the way in: AMI's
    // firmware serves both at one address), letting in the sessions `admits` says that page made (by the Cookie
    // header as it came) and taking the passes `takesPass` says it handed out (each once). Everything else asked
    // there is that stand-in's.
    void servesAt(StandIn& other, std::function<bool(const QByteArray&)> admits,
                  std::function<bool(const QByteArray&)> takesPass)
    {
        m_admits = std::move(admits);
        m_takesPass = std::move(takesPass);
        other.upgrade = [this](QTcpSocket* socket, const QByteArray& request) { return letIn(socket, request); };
    }

    // Takes nothing more off any of its lines: Windows keeps a little of what comes, and then nothing more is taken
    // from whoever says it.
    void stall()
    {
        for (const QPointer<QTcpSocket>& under : m_handed) {
            if (under != nullptr) {
                under->setReadBufferSize(1024);
                under->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
                under->blockSignals(true);
            }
        }
    }
    // ...and takes it again, what waited first. (A line that fell meanwhile said so to nobody: it is closed here, and
    // its seat ends with it.)
    void readAgain()
    {
        for (const QPointer<QTcpSocket>& under : m_handed) {
            if (under != nullptr) {
                under->setReadBufferSize(0);
                under->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
                under->blockSignals(false);
                emit under->readyRead();
            }
        }
        for (const auto& [line, under] : m_lines) {
            if (line != nullptr && (under == nullptr || under->state() != QAbstractSocket::ConnectedState)) {
                line->abort();
                line->deleteLater();
            }
        }
    }
    // What it said that has not left for whoever it said it to (Windows took the rest), on all its lines: what a viewer
    // that takes nothing off its line leaves waiting here.
    qint64 waitingToGo() const
    {
        qint64 waiting = 0;
        for (const auto& line : m_lines) {
            if (line.second != nullptr) {
                waiting += line.second->bytesToWrite();
            }
        }
        return waiting;
    }

    // The keyboard reports it took, and the mouse reports.
    std::vector<Hid> keyboards() const
    {
        std::vector<Hid> found;
        std::ranges::copy_if(hid, std::back_inserter(found), [](const Hid& one) { return one.keyboard; });
        return found;
    }
    std::vector<Hid> mice() const
    {
        std::vector<Hid> found;
        std::ranges::copy_if(hid, std::back_inserter(found), [](const Hid& one) { return !one.keyboard; });
        return found;
    }
    int heardOf(std::uint16_t kind) const
    {
        int found = 0;
        for (const Heard& one : heard) {
            found += one.kind == kind ? 1 : 0;
        }
        return found;
    }

    // A frame as the console sends it: its facts with the first piece, and its data cut up.
    std::vector<bmc::ami::Bytes> pieces(const bmc::aspeed::frames::Frame& frame) const
    {
        const bmc::aspeed::FrameHeader& header = frame.header();
        const std::vector<std::uint8_t> data = frame.data();
        bmc::ami::Bytes facts(bmc::ami::frameFacts);
        auto put16 = [&facts](std::size_t at, int value) {
            facts[at] = static_cast<std::uint8_t>(value);
            facts[at + 1] = static_cast<std::uint8_t>(value >> 8);
        };
        put16(4, header.width);
        put16(6, header.height);
        put16(13, header.gridWidth);
        put16(15, header.gridHeight);
        facts[44] = static_cast<std::uint8_t>(header.strength);
        facts[45] = header.lumaForColour ? 1 : 0;
        facts[47] = static_cast<std::uint8_t>(header.correctionStrength);
        facts[55] = header.halfColour ? 1 : 0;
        facts[69] = static_cast<std::uint8_t>(data.size());
        facts[70] = static_cast<std::uint8_t>(data.size() >> 8);
        facts[71] = static_cast<std::uint8_t>(data.size() >> 16);
        std::vector<bmc::ami::Bytes> found;
        for (std::size_t at = 0, number = 0; at < data.size() || number == 0;
             at += static_cast<std::size_t>(piece), ++number) {
            bmc::ami::Bytes body = {static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(number >> 8)};
            if (number == 0) {
                body.insert(body.end(), facts.begin(), facts.end());
            }
            const std::size_t to = std::min(data.size(), at + static_cast<std::size_t>(piece));
            body.insert(body.end(), data.begin() + static_cast<std::ptrdiff_t>(std::min(at, data.size())),
                        data.begin() + static_cast<std::ptrdiff_t>(to));
            found.push_back(bmc::ami::message(bmc::ami::kind::video, 0, body));
        }
        return found;
    }

private:
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

    static QByteArray bytesOf(const bmc::ami::Bytes& bytes)
    {
        return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
    }

    // A keyboard or mouse report the console was sent, decoded; why not, where it does not check out. The layout is
    // the controller's (bmc/ami_console.h): "IUSB" and four blanks, a header of 32 bytes that sums to zero, one byte
    // of report length, the USB HID report (the web's decode_hid).
    static std::optional<Hid> decoded(const bmc::ami::Bytes& body, QByteArray& why)
    {
        static constexpr std::string_view signature = "IUSB    ";
        if (body.size() < 33 || !std::equal(signature.begin(), signature.end(), body.begin())) {
            why = "no IUSB signature";
            return std::nullopt;
        }
        unsigned sum = 0;
        for (std::size_t at = 0; at < 32; ++at) {
            sum += body[at];
        }
        if ((sum & 0xFF) != 0) {
            why = "the header does not sum to zero";
            return std::nullopt;
        }
        const std::size_t length = body.size() - 33;
        const auto four = [&body](std::size_t at) {
            return static_cast<std::uint32_t>(body[at]) | static_cast<std::uint32_t>(body[at + 1]) << 8 |
                   static_cast<std::uint32_t>(body[at + 2]) << 16 | static_cast<std::uint32_t>(body[at + 3]) << 24;
        };
        if (body[32] != length) {
            why = "the report length byte is wrong";
            return std::nullopt;
        }
        if (body[8] != 1 || body[9] != 0 || body[10] != 32 || four(12) != length + 1) {
            why = "the header's version, length or data length is wrong";
            return std::nullopt;
        }
        const bool keyboard = body[17] == 0x30;
        const bool mouse = body[17] == 0x31;
        if (body[19] != 0x80 || body[20] != 2 || !(keyboard || mouse) || body[18] != (keyboard ? 0x10 : 0x20) ||
            body[21] != (keyboard ? 0 : 1)) {
            why = "the header's direction, protocol, device or interface is wrong";
            return std::nullopt;
        }
        Hid one;
        one.number = four(24);
        const std::uint8_t* report = body.data() + 33;
        if (keyboard) {
            if (length != 8) {
                why = "a keyboard report that is not eight bytes";
                return std::nullopt;
            }
            one.bits = report[0];
            for (std::size_t at = 2; at < 8; ++at) {
                if (report[at] != 0) {
                    one.keys.push_back(report[at]);
                }
            }
            return one;
        }
        const auto signedByte = [](std::uint8_t byte) { return byte < 0x80 ? int{byte} : byte - 0x100; };
        one.keyboard = false;
        one.bits = report[0];
        if (length == 4) {
            one.relative = true;
            one.x = signedByte(report[1]);
            one.y = signedByte(report[2]);
            one.wheel = signedByte(report[3]);
            return one;
        }
        if (length != 6) {
            why = "a mouse report of an unexpected size";
            return std::nullopt;
        }
        one.x = report[1] | report[2] << 8;
        one.y = report[3] | report[4] << 8;
        one.wheel = signedByte(report[5]);
        return one;
    }

    // One console session.
    struct Seat {
        bmc::ami::Messages arriving;
        bool sent = false;
        bool complete = false;
        bool waiting = false;
        QElapsedTimer began;
        QElapsedTimer quiet;
        QElapsedTimer asked;
        QTimer ticks;
        QPointer<QTcpSocket> socket; // under the WebSocket, for a console that stops taking anything off it
        bool deafened = false;
        QElapsedTimer passed;     // since the pass was taken
        bool keysDown = false;    // by the last keyboard report
        bool buttonsDown = false; // ...and the last mouse report
    };

    void serve(QWebSocket* line)
    {
        connect(line, &QWebSocket::disconnected, line, &QObject::deleteLater);
        auto send = [line](const bmc::ami::Bytes& bytes) { line->sendBinaryMessage(bytesOf(bytes)); };
        if (watching >= seats) {
            send(bmc::ami::message(bmc::ami::kind::tooMany));
            line->close();
            return;
        }
        ++watching;
        mostWatching = std::max(mostWatching, watching);
        ++sessions;
        auto* seat = new Seat; // NOLINT(cppcoreguidelines-owning-memory): the line's, gone with it
        connect(line, &QObject::destroyed, this, [this, seat] {
            --watching;
            endedWithSomethingDown += seat->keysDown || seat->buttonsDown ? 1 : 0;
            delete seat; // NOLINT(cppcoreguidelines-owning-memory)
        });
        for (const QPointer<QTcpSocket>& under : m_handed) {
            if (under != nullptr && under->peerPort() == line->peerPort()) {
                seat->socket = under;
            }
        }
        m_lines.emplace_back(line, seat->socket);
        seat->began.start();
        seat->quiet.start();
        send(bmc::ami::message(bmc::ami::kind::allowed, 2));
        for (const bmc::ami::Bytes& first : saysFirst) {
            send(first);
        }
        // What is said to it is taken off its line and heard by nobody, a goodbye too: it is answered never. (Taken
        // off all the same, so that the line's end is seen when it comes.)
        const auto deafen = [seat] {
            if (!seat->deafened && seat->socket != nullptr) {
                QObject::disconnect(seat->socket, &QIODevice::readyRead, nullptr, nullptr);
                connect(seat->socket, &QIODevice::readyRead, seat->socket,
                        [under = seat->socket] { (void)under->readAll(); });
                (void)seat->socket->readAll();
                seat->deafened = true;
            }
        };
        connect(&seat->ticks, &QTimer::timeout, line, [this, line, seat, send, deafen] {
            if (deaf) {
                deafen();
                return;
            }
            if (sessionLastsMs > 0 && seat->began.elapsed() > sessionLastsMs) {
                send(bmc::ami::message(bmc::ami::kind::stop, bmc::ami::timeUp));
                seat->ticks.stop();
                line->close();
                return;
            }
            while (seat->sent && !asks.empty()) {
                const auto [kind, who] = asks.front();
                asks.erase(asks.begin());
                seat->asked.start();
                send(bmc::ami::message(
                    kind, bmc::ami::sharing::asked,
                    {reinterpret_cast<const std::uint8_t*>(who.constData()), static_cast<std::size_t>(who.size())}));
            }
            if (seat->sent && !says.empty()) {
                while (!says.empty()) {
                    send(says.front());
                    says.erase(says.begin());
                }
                if (hangsUp) {
                    seat->ticks.stop();
                    line->close();
                    return;
                }
                if (deafAfterSaying) {
                    deaf = true;
                    deafen();
                    return;
                }
            }
            if (!mute && seat->quiet.elapsed() >= stillThereEveryMs) {
                seat->quiet.restart();
                send(bmc::ami::message(bmc::ami::kind::stillThere));
            }
        });
        seat->ticks.start(50);
        connect(line, &QWebSocket::binaryMessageReceived, this, [this, line, seat, send](const QByteArray& data) {
            seat->quiet.restart();
            const auto found = seat->arriving.feed(
                {reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<std::size_t>(data.size())});
            if (!found) {
                line->close();
                return;
            }
            for (const bmc::ami::Message& one : found.value()) {
                heard.push_back({one.kind, one.status, static_cast<qsizetype>(one.body.size())});
                take(line, *seat, one, send);
            }
        });
    }

    // A line that asks for the console at /kvm, let in as a WebSocket where it comes with a session of its web page.
    bool letIn(QTcpSocket* socket, const QByteArray& request)
    {
        if (!request.startsWith("GET /kvm ")) {
            return false;
        }
        ++askings;
        origins.append(headerIn(request, "origin").value_or("(none)"));
        const QByteArray cookie = headerIn(request, "cookie").value_or(QByteArray());
        if (m_admits ? !m_admits(cookie) : cookie != "QSESSIONID=" + session) {
            return false; // answered as any question: not here for whoever is not signed in
        }
        if (silent) {
            return true; // taken, and never answered
        }
        m_handed.append(socket);
        QTimer::singleShot(0, &m_server, [this, socket = QPointer<QTcpSocket>(socket)] {
            if (socket != nullptr) {
                m_server.handleConnection(socket);
                emit socket->readyRead();
            }
        });
        return true;
    }

    template <typename Send>
    void take(QWebSocket* line, Seat& seat, const bmc::ami::Message& one, const Send& send)
    {
        namespace ami = bmc::ami;
        if (one.kind == ami::kind::complete) {
            seat.complete = true;
        } else if (one.kind == ami::kind::stop) {
            line->close();
        } else if (one.kind == ami::kind::pass) {
            if (reconnects && !seat.complete) {
                line->close(); // not how this firmware is greeted
                return;
            }
            QByteArray shown(reinterpret_cast<const char*>(one.body.data()) + 1, 129);
            shown.truncate(shown.indexOf('\0'));
            const bool good = m_takesPass ? m_takesPass(shown) : passes.contains(shown);
            const ami::Bytes held(heldBy.begin(), heldBy.end());
            if (good) {
                // Who else is there is said before the word on the pass, as a real board says it.
                if (holderReturning) {
                    send(ami::message(ami::kind::sharing, ami::sharing::answered | ami::answer::holderReturning << 8));
                } else if (!heldBy.isEmpty() && shutOut) {
                    seat.waiting = true;
                    send(ami::message(ami::kind::sharing, ami::sharing::answered | ami::answer::shutOut << 8, held));
                } else if (!heldBy.isEmpty()) {
                    seat.waiting = true;
                    send(ami::message(ami::kind::sharing, ami::sharing::waiting, held));
                }
            }
            send(ami::message(ami::kind::passSeen, firstSession ? ami::firstSession : 0,
                              ami::Bytes{static_cast<std::uint8_t>(good ? 1 : 0), 0}));
            if (!good) {
                line->close();
                return;
            }
            seat.passed.start();
            passes.remove(shown); // a pass is for one session
            const bool alone = heldBy.isEmpty() && !holderReturning;
            ami::Bytes both = ami::message(ami::kind::licence);
            const ami::Bytes others = ami::message(ami::kind::others, 0, ami::Bytes(alone ? 134 : 268));
            both.insert(both.end(), others.begin(), others.end());
            send(both);
            send(ami::message(ami::kind::mouseMode, 0, ami::Bytes{mouseMode})); // which mouse the server shows
            if (alone) {
                send(ami::message(ami::kind::nextHolder, ami::sharing::becameMaster)); // "the console is yours"
            }
        } else if (one.kind == ami::kind::hid) {
            QByteArray why;
            std::optional<Hid> report = decoded(one.body, why);
            if (!report) {
                badHid.append(why);
            } else if (firstSession && (!seat.passed.isValid() || seat.passed.elapsed() < hidSettlesMs)) {
                ++hidLost; // (the keyboard and mouse are not attached yet)
            } else {
                report->atMs = m_made.elapsed();
                (report->keyboard ? seat.keysDown : seat.buttonsDown) = report->bits != 0 || !report->keys.empty();
                hid.push_back(std::move(*report));
            }
        } else if (one.kind == ami::kind::sharing || one.kind == ami::kind::nextHolder) {
            answers.push_back(
                {one.kind, one.status,
                 QByteArray(reinterpret_cast<const char*>(one.body.data()), static_cast<qsizetype>(one.body.size())),
                 seat.asked.isValid() ? seat.asked.elapsed() : -1});
        } else if (one.kind == ami::kind::resume && beginsWithNothing && !seat.waiting) {
            // A real board, to a console opened again: a frame that says nothing changed, and the screen only when
            // it is asked for the whole of it, a moment later.
            if (!screen.empty()) {
                const bmc::aspeed::frames::Frame nothing(screen.front().header().width, screen.front().header().height);
                for (const ami::Bytes& part : pieces(nothing)) {
                    send(part);
                }
            }
        } else if ((one.kind == ami::kind::resume || one.kind == ami::kind::fullScreen) && !seat.sent &&
                   !seat.waiting) {
            seat.sent = true;
            if (beginsWithNothing) {
                QTimer::singleShot(600, line, [this, line, send] { sendScreen(line, send); });
            } else {
                sendScreen(line, send);
            }
        }
    }

    template <typename Send>
    void sendScreen(QWebSocket* line, const Send& send)
    {
        if (sendsFrameWhen && !screen.empty()) {
            for (const bmc::ami::Bytes& part : pieces(screen.front())) {
                send(part);
            }
            sendWhenSaid(line, send, 1);
            return;
        }
        for (std::size_t at = 0; at < screen.size(); ++at) {
            if (frameEveryMs > 0 && at > 0) {
                QTimer::singleShot(static_cast<int>(at) * frameEveryMs, line, [this, send, frame = screen[at]] {
                    for (const bmc::ami::Bytes& part : pieces(frame)) {
                        send(part);
                    }
                });
                continue;
            }
            for (const bmc::ami::Bytes& part : pieces(screen[at])) {
                send(part);
            }
        }
    }

    // Frame `at` of `screen` and those after it, each once sendsFrameWhen says so.
    template <typename Send>
    void sendWhenSaid(QWebSocket* line, const Send& send, std::size_t at)
    {
        if (at >= screen.size()) {
            return;
        }
        QTimer::singleShot(10, line, [this, line, send, at] {
            if (!sendsFrameWhen || !sendsFrameWhen(at)) {
                sendWhenSaid(line, send, at);
                return;
            }
            for (const bmc::ami::Bytes& part : pieces(screen[at])) {
                send(part);
            }
            sendWhenSaid(line, send, at + 1);
        });
    }

    QWebSocketServer m_server;
    QList<QPointer<QTcpSocket>> m_handed;
    // Of a console served at another stand-in too (servesAt): whom it lets in, and which passes it takes.
    std::function<bool(const QByteArray&)> m_admits;
    std::function<bool(const QByteArray&)> m_takesPass;
    // Each line, and the socket under it.
    std::vector<std::pair<QPointer<QWebSocket>, QPointer<QTcpSocket>>> m_lines;
    QElapsedTimer m_made = [] {
        QElapsedTimer made;
        made.start();
        return made;
    }();
};

} // namespace qkvm::core::testing
