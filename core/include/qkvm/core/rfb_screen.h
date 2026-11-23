#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/bmc/aspeed.h>
#include <qkvm/core/device_line.h>
#include <qkvm/core/server_console.h>
#include <qkvm/core/typed_keys.h>
#include <qkvm/core/wiped.h>

namespace qkvm::core {

class RfbReader;

// A server's own screen from its controller's console where the controller runs OpenBMC: bmcweb's WebSocket `/kvm/0`
// (a DeviceLine), signed in to with the driver's Redfish session (its X-Auth-Token), carrying RFB, which obmc-ikvm
// speaks (read by rfb/). As the web's app/qkvm/openbmc.py (OpenBmcScreen) over vncdevice.py's DoorScreen, with its
// manners:
//
// * A session is held only while somebody looks (look(), a card's still, keys), and let go of Limits::idleMs after the
//   last of them: each is one of the controller's four seats, and none is held for nobody. One session at a time: the
//   next is not opened before the last one's line has gone.
// * The screen is asked for one update at a time: once a second while it is looked at, a viewer's pace while it is
//   live (liveSeen); a server whose screen does not change answers nothing, so after Limits::heartbeatMs without an
//   update one pixel is asked for outright, which any server answers within Limits::messageWithinMs, or the session is
//   over ("timeout"). The handshake has Limits::handshakeMs; a message begun has Limits::messageWithinMs to be whole,
//   and its next piece Limits::pieceWithinMs.
// * A session that came to nothing is asked for again later and later (Limits::retryMs, the controller's console
//   ladder); what obmc-ikvm sends that RFB as this program reads it is not, or a refusal, is said and left alone on
//   the same ladder. A session that served Limits::settledMs and then ended is no failure: the next is asked for at
//   once.
// * No password goes over RFB but one the way in hands in for its session (WayIn::password, with
//   WayIn::vncAuthentication): obmc-ikvm asks for none, and OpenBMC's way lets no server ask, so one that asks for VNC
//   authentication is answered nothing (the only password there is the controller's own, which goes to its sign-in
//   alone). A door that hands one in (a Proxmox guest's ticket) has the challenge answered with it, once.
//
// Keys and the pointer go over the same session, as RFB's key and pointer events (the web's vncdevice.ScreenInput):
// * A turn is a session: keys are given (askForInput) once its handshake is over and Limits::inputAfterMs has passed
//   (obmc-ikvm drops what comes in a client's first frames), waited for at the most Limits::inputWaitMs, and the turn
//   ends with its session. Asking keeps the session wanted, as looking does.
// * The whole keyboard of a live is said as the key events that change it: what is let go of first, keys before
//   modifiers, then what goes down; each key by its keysym as the key is without Shift (rfb::keysymFor), and a key that
//   has none goes nowhere. The pointer at its place in the screen's pixels, with RFB's buttons (1 left, 2 middle, 4
//   right); a notch of the wheel a press of its button (8 up, 16 down) and its letting go.
// * What is down is let go of before a session is let go of from here.
// * Typings (an action's keys, a text) as the web types them for obmc-ikvm: a combination pressed in its order and let
//   go of in the other; a text character by character, by its keysym on the US layout, Shift down around those that
//   layout types with it (obmc-ikvm holds Shift only between a Shift key's own events), and what that layout has no key
//   for refused before anything goes. In pieces of Limits::keysAtOnce events that leave nothing down, whoever asked
//   asked before each whether they may still have it typed.
//
// Frames are read off this thread (RfbReader), and what changed of the screen handed to Shown as AMI's console hands
// it; the card's still is made there too. On the thread it was made on.
class RfbScreen : public ServerConsole {
    Q_OBJECT

public:
    // The way into one console session.
    struct WayIn {
        QUrl address; // the controller's: https or http, its host, its port where it has one of its own
        bool knownByCertificate = false;
        QByteArray certificate;                 // its SHA-256
        QByteArray token;                       // the driver's Redfish session, as X-Auth-Token says it
        QByteArray path = QByteArray("/kvm/0"); // where its console is, as its manager announces it
        // RFB straight on the connection, as a VNC server speaks it (DeviceLine::receiveRaw): `address` is then http
        // (vnc://) or https (vncs://, TLS from the first byte), its host and its port; nothing is asked, and neither
        // `path` nor `token` goes anywhere.
        bool raw = false;
        // In place of the token: who asks as an Authorization header says it (a Proxmox guest's console, its API token:
        // DeviceLine::Where::authorization); and the subprotocol the console is asked for and must answer with
        // (Proxmox's "binary"; none: none, as OpenBMC's). The token is a secret's bytes, written over when the way
        // goes, on every road (one that comes when nobody waits for it among them); the session's line takes a copy of
        // its own, which it writes over once its upgrade went (DeviceLine).
        SecretBytes authorization;
        QByteArray protocol;
        // VNC authentication: whether the server may ask for it (else a server that asks is one that asks for a way
        // this program does not speak, and is sent nothing), and the password it is answered with for this session
        // (a door's ticket, a VNC server's saved password). A server that asks where no password is handed in is
        // sent none ("no_password"). The password goes to the session's reader, which answers the challenge with it
        // (vnc_auth.h) and lets go of it then, or once the handshake is over; it is in no log and no error.
        bool vncAuthentication = false;
        SecretText password;
        // Asked the moment before the answer to the challenge goes to the server (the web's before_password): for
        // whoever counts the passwords that went (SignInPacing), and holds them to one at a time when they go, not
        // only when the way was made (outside review 2.4: two drivers of one server both made their way before either
        // had sent). An error, and the answer does not go: the session ends in its words. On this object's thread.
        std::function<base::Result<void>()> passwordSent;
        // Called once when the session this way was for is over and its line has gone, on every road. On this
        // object's thread; it may be called after whoever made the way is gone (it holds only weak references).
        std::function<void()> done;
    };
    using Answer = std::function<void(base::Result<WayIn>)>;
    using Wanted = std::function<bool()>;
    // Asks for a way in, and is answered once, now or later: a way, or why there is none. Never asked again while it
    // has not answered. `wanted` says whether the way is still waited for.
    using Asker = std::function<void(Answer, Wanted)>;

    // What the console is held to, as the web's (vncdevice.py, openbmc.py, redfish.py's CONSOLE_*). A test makes the
    // times short.
    struct Limits {
        int idleMs = 60000;      // without anybody looking, after which the session is let go of (LET_GO_AFTER)
        int openingMs = 12000;   // for the line to be let in (CONSOLE_OPENING)
        int handshakeMs = 20000; // for the whole handshake (HANDSHAKE_WAIT)
        int lookPaceMs = 1000;   // between two requests while it is looked at (OpenBmcScreen.LOOK_PACE)
        int livePaceMs = 40;     // ...while somebody has it live (LIVE_PACE)
        int liveForMs = 2500; // after a live last asked, the pace is still a viewer's (LIVE_FOR, and the live's look)
        int heartbeatMs = 25000;     // without an update, one pixel is asked for outright (DoorScreen.HEARTBEAT_AFTER)
        int messageWithinMs = 60000; // a message, and the answer to that pixel, to come whole (MESSAGE_WITHIN)
        int readerQuietMs =
            60000;                // the reader telling nothing this long while its queue holds the line: not waited for
        int pieceWithinMs = 8000; // the next piece of a message begun (READ_WAIT)
        std::array<int, 4> retryMs = {30000, 60000, 120000, 300000}; // after one that came to nothing (CONSOLE_RETRY)
        int settledMs = 30000;      // a session that served this long and ended is no failure (SETTLED)
        int tickMs = 250;           // how often the times above are looked at
        int goodbyeWithinMs = 1000; // for the line to go once it is closed from here
        // Keys and the pointer: obmc-ikvm takes none in a client's first frames (INPUT_AFTER), and they wait for a
        // session that takes them at the most this long (OPENBMC_WAIT).
        int inputAfterMs = 1500;
        int inputWaitMs = 10000;
        int keysAtOnce = 32;  // key events said in one piece (KEYS_AT_ONCE)
        int piecePauseMs = 2; // between two pieces of a typing
        // The longest message of the console's WebSocket (OPENBMC_MOST), and what may wait unread in this program
        // before nothing more is taken off the line.
        qint64 largestMessageBytes = qint64{1024} * 1024;
        qint64 heldBackFromBytes = qint64{8} * 1024 * 1024;
        qint64 mostWaitingBytes = qint64{64} * 1024; // to go to the console (DeviceLine::Limits::mostUnsentBytes)
        // The drawing share the reader is kept to (DRAWING_SHARE, DRAWING_BURST; rfb::Options): what of one drawing
        // thread its pictures may take, a second per 1000, and at once. A test makes it small to see the pauses.
        int drawingSharePerMil = 500;
        int drawingBurstMs = 1000;
        // A family that holds its session for as long as its machine is on the wall (a VNC server: the web's
        // VncDevice, held while kept, keep()): while nobody has looked for watchedForMs (KEEPALIVE_IDLE), the next
        // update is asked for at idlePaceMs (IDLE_PACE). 0: the looking pace always (a door's console, held only while
        // somebody looks).
        int idlePaceMs = 0;
        int watchedForMs = 30000;
        // How a text is typed (the web's US_SHIFT): true, each character by its keysym on the US layout with Shift
        // down around those that layout types with it, and what that layout has no key for refused (obmc-ikvm); false,
        // each character by its own keysym (rfb::keysymOf), which the server's own layout makes a key (a VNC server),
        // and only what no keysym says refused, by its place, before anything goes.
        bool usShift = true;
        // What the screen is called in what it says (the web's THING): empty, a server's console behind its
        // controller ("The server's console", "The controller's console"); else this, as "the VNC server".
        QString thing;
    };
    // How long after a session that came to nothing (its code, whether it had served a while, and how long it was let
    // in: -1 where it never was) the next may be asked for, where a family keeps a ladder of its own (a VNC server's,
    // the web's): in place of `retryMs`.
    using Hold = std::function<qint64(const QString& code, bool served, qint64 lastedMs)>;

    RfbScreen(Asker asker, Shown shown, Limits limits, QObject* parent = nullptr);
    // Lets go of a session that is open, as close().
    ~RfbScreen() override;
    RfbScreen(const RfbScreen&) = delete;
    RfbScreen& operator=(const RfbScreen&) = delete;
    RfbScreen(RfbScreen&&) = delete;
    RfbScreen& operator=(RfbScreen&&) = delete;

    base::Result<void> look() override;
    void liveSeen() override;
    const bmc::aspeed::Picture& picture() const noexcept override { return m_picture; }
    void still(int width, StillAnswer done) override;
    void close() override;

    void askForInput(InputAnswer answer) override;
    base::Result<void> keyboard(const typed::Report& keys, Turn turn) override;
    base::Result<void> mouse(std::uint8_t buttons, int x, int y, int wheel, Turn turn) override;
    base::Result<void> inTurn(Turn turn) const override;
    void press(const QString& keys, Still still, Typed done) override;
    void type(const QString& text, Still still, Typed done, bool thenEnter = false) override;
    void stopTyping() override;

    // What the server said of itself in the session that is open (its name), and its version: empty before.
    QString serverName() const;
    // The session is held as look() holds it, without anybody looking: its pace is the idle one (Limits::idlePaceMs).
    // Ok once the handshake of the session that is open is over; otherwise as look() says.
    base::Result<void> keep();
    // The handshake of the session that is open is over, and the screen's size as the server said it then.
    bool letIn() const;
    int width() const;
    int height() const;
    // The RFB version the session that is open speaks: 3, 7 or 8 (of 3.x); 0 before its handshake is over.
    int rfbMinor() const;
    // How long the session that is open has been let in (ms); -1 before its handshake is over.
    qint64 letInForMs() const;
    // The next session may be asked for now, whatever wait stands after the last one (a family's, for whoever watches
    // it enlarged during a wait of its own: a VNC server's run of ends). Nothing where none stands.
    void retryNow();
    // Why the last session came to nothing, for as long as the next may not be asked for yet.
    std::optional<base::Error> standing() const;
    // The family's own ladder (Hold), in place of Limits::retryMs.
    void setHold(Hold hold) { m_hold = std::move(hold); }
    // How the last session that ended came to its end, as `ended` is said, for a family that tells a failure of contact
    // by it (a Proxmox guest's: review cx3-3, L3): no line was made for it (its way in failed, whose questions are
    // counted as they fail, or it was given back first); its line ended before the server let it in; it fell after
    // that, without the server's goodbye; or it ended otherwise after that (the server's goodbye, what the server said,
    // a limit of this program's, given back).
    enum class Ended { NoLine, NotLetIn, Fell, Otherwise };
    Ended lastEnded() const { return m_lastEnded; }

signals:
    // The handshake of a session is over: the server let this program in (a password that went with it was taken).
    void handshakeOver();
    // Over TLS, the certificate a session's line was let in by (DeviceLine::certificateShown).
    void certificateShown(const QByteArray& der);

private:
    struct Session;
    struct Typing;
    struct Asking {
        InputAnswer answer;
        qint64 until = 0;
        std::uint64_t typing = 0; // the typing it was asked for (Typing::number); 0: whoever else
    };
    struct StillAsked {
        std::uint64_t number = 0;
        StillAnswer done;
    };

    void open();
    // look() and keep(): the session wanted, watched or only kept.
    base::Result<void> want(bool watching);
    // The pace the next update is asked for at now: a viewer's, a looker's, or (nobody watching) the idle one.
    int paceNow() const;
    // What it says, in its own words (Limits::thing): the sentence a console behind a controller says, or the reason
    // after "The VNC server: ".
    QString said(const QString& ofAConsole, const QString& reason) const;
    void entered(WayIn way);
    void heard(const QByteArray& bytes);
    void takeReads();
    void tick();
    void pace();
    // The share pause has ended (its single-shot, or a tick): let the line go and ask again.
    void resumeShare();
    void request(bool incremental, bool onePixel);
    bool say(const QByteArray& bytes);
    void lineEnded(DeviceLine::End why);
    void leave();
    // The session is over, for this reason (an empty code: nobody looked); what follows from it for the next. `alone`:
    // what it sent is not taken, or it refused: left alone on the ladder as a refusal.
    void finish(const QString& code, const QString& message);
    // ...for the end of its line, which is the reason (said in the line's words: saidOfTheLine).
    void finishForTheLine(DeviceLine::End why);
    qint64 now() const { return m_clock.elapsed(); }
    bool live() const;
    // Keys and the pointer: whoever asked and may be answered now is; what is down let go of on the session; a key
    // event said; the typings' next step and end.
    void answerInput();
    // askForInput, of a typing (`typing`, its number) or of whoever else (0).
    void askForInput(InputAnswer answer, std::uint64_t typing);
    void letGoOfWhatIsDown(Session& session);
    void queueTyping(std::unique_ptr<Typing> typing);
    void typingStep();
    void endTyping(const base::Result<void>& how);

    Asker m_asker;
    Shown m_shown;
    Limits m_limits;
    QElapsedTimer m_clock;
    QTimer m_ticks;
    QTimer m_paces;
    QTimer m_shareWait; // single-shot: fires when a share pause ends, so a pause of a few ms costs no more
    std::unique_ptr<Session> m_session;
    QPointer<QObject> m_leaving;
    bmc::aspeed::Picture m_picture;
    std::vector<StillAsked> m_stillsAsked;
    std::uint64_t m_stillsMade = 0;
    bool m_asking = false;
    bool m_closed = false;
    qint64 m_wantedAt = 0;
    qint64 m_keptAt = -1;
    qint64 m_liveAt = -1;
    Hold m_hold;
    // What the last session came to, and when the next may be asked for; how it ended (lastEnded), and the end of its
    // line where that is what finish() is told of.
    Ended m_lastEnded = Ended::NoLine;
    std::optional<DeviceLine::End> m_forTheLine;
    std::optional<base::Error> m_refusal;
    qint64 m_retryAt = 0;
    int m_failures = 0;
    // Keys and the pointer: why the last turn ended, who waits for one, and the typings, the first under way.
    base::Error m_lost{"dropped", "The server's console was closed"};
    std::vector<Asking> m_askings;
    QTimer m_inputTicks;
    std::deque<std::unique_ptr<Typing>> m_typings;
    std::uint64_t m_typingsMade = 0;
    QTimer m_typingSteps;
};

} // namespace qkvm::core
