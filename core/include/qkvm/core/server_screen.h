#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <qkvm/base/result.h>
#include <qkvm/bmc/ami_console.h>
#include <qkvm/bmc/aspeed.h>
#include <qkvm/core/device_line.h>
#include <qkvm/core/typed_keys.h>

namespace qkvm::core {

class ScreenReader;

// A server's own screen, from its management controller's console (AMI's firmware, MegaRAC): the console its own
// web page opens, a WebSocket at /kvm (a DeviceLine) that speaks AMI's protocol, with the screen in ASPEED's
// format (both read by bmc/). As the web's app/qkvm/redfish.py (its _Console, _run_console, _watch_console and
// _console_with_a_picture), with its manners, which matter most: whoever only looks is never in a person's way.
//
// * A console is held open only while somebody looks (look()), and given back after Limits::idleMs without.
// * Somebody at the controller's own page who asks for the console is let in at once, keys and all, and the wall
//   goes on looking; at most Limits::letInAtMost times a session (a person asks once).
// * Somebody who had it first is not asked for it: told to wait, the console is left at once, and not asked for
//   again before Limits::heldMs ("console_held").
// * After a session that came to nothing, the next is asked for later and later (Limits::retryMs, then
//   Limits::giveUpMs for what asking again does not change). A session that showed the screen for
//   Limits::settledMs and was then ended by the controller (its time was up) is opened again at once.
//
// What a session needs to be opened comes from the controller's web page (its sign-in and the pass it hands out:
// the driver's), asked for each session anew (a pass is good for one): the Asker. The screen goes out through the
// Shown callback: the picture as it stands and the rectangles that changed since it was last shown. Frames are read in
// the program's own process (the owner's decision, 2026-10-29, as the KVMs' H.264), on a thread of their own and not
// this object's (ScreenReader, review 3's HIGH 1: a busy 1080p frame took about 46 ms of the interface thread): this
// takes them off the console's line, one session's in their order, and is handed back the screen as it stands. Two
// frames wait to be read at the most: while they do, nothing more is taken off the line (DeviceLine::holdBack; the
// controller is held back by that, as the web's was while it read). One whose thread was busy is shown, when it comes
// back, the screen as the newest frame left it with all that changed meanwhile: the frames read in between are one
// picture to it, none of what they changed is lost, and what its live hands on stays whole and in order. The card's
// still is made on that thread too (still()).
//
// Keys and the mouse go over the same console (part 4; the web's redfish.py 1310-1512 and its live.RedfishHid), as
// the reports of the server's virtual USB keyboard and mouse (ami::keyboard, ami::mouseAbsolute), and with manners
// of their own:
// * Nothing goes into a console that is somebody else's: only into a session of this program's that the controller
//   has said "the console is yours" to, and has given to nobody since; one that is not told so within
//   Limits::masterWaitMs of its pass being taken is somebody else's ("in_use"). That is one TURN of the console's
//   keyboard and mouse. It ends when the console is given to somebody at the controller's own page, is switched to
//   another session, or ends; what this program has down on the server is let go of first, where the console is
//   still this program's, and nothing more of that turn goes ("in_use" once it was given to somebody here).
// * Nothing in the first Limits::keysSettleMs of a first session (the controller attaches its keyboard and mouse to
//   the server only then, and a key before is lost); keys wait at most Limits::inputWaitMs for a console that is open
//   and this program's.
// * Nothing is said behind what has waited Limits::inputSayMs to leave: a key from then would be pressed now. A
//   report that only lets go (of a key, a modifier or a button, and presses none) is said all the same, behind what
//   waits: it presses nothing. Where it has not left within Limits::letGoWithinMs, while the line goes on, the
//   session is ended (the controller lets go of what a session had down as it ends) and said so ("dropped": a key
//   may have stayed down meanwhile).
// * Nothing stays down: what this program has down is let go of with the turn, before the console is given away,
//   at once where it was switched to another session, before the goodbye where the screen is closed or thrown away,
//   and where the server's mouse becomes a relative one (a relative report then, as the web's); a typing stopped with
//   a key down lets go of it at once.
// * A combination is held Limits::holdMs; a text's keys go Limits::keyPauseMs apart, and whoever asked is asked
//   again every Limits::askAgainEvery keys whether they may still have it typed.
// The keys are the program's own: a live line's by their USB numbers, as input/keys.h gives them (typed::Keyboard
// makes the whole keyboard of them, typed::buttonBit a button), an action's by name or as a text (typed_keys.h).
class ServerScreen : public QObject {
    Q_OBJECT

public:
    // The way into one console session.
    struct WayIn {
        QUrl address; // the controller's: https or http, its host, its port where it has one of its own
        bool knownByCertificate = false;
        QByteArray certificate;       // its SHA-256
        QByteArray cookie;            // its web page's session, as one cookie: QSESSIONID=...
        QByteArray pass;              // the console pass it handed out for this session
        QByteArray ownAddress;        // this PC's, as the controller saw it
        QByteArray controllerAddress; // the controller's, as it names itself
        QByteArray webSession;        // the number of its web page's session
        // Its firmware says it lets a console reconnect (it is then greeted "the connection is complete" first);
        // not known: as found out by trying (a console that closes on the greeting is greeted the other way).
        std::optional<bool> completeFirst;
        // Called once when the session this way was for is over and its line has gone, on every road (it ended,
        // the screen was closed or thrown away, the pass could not be said, or the way came when nobody waited for
        // it any more): whoever made the way gives its web page's session back then. On this object's thread.
        // A line that is still going when the screen is thrown away is kept by the program until it has gone, so
        // this may be called after whoever made the way is gone: it holds only weak references (a QPointer, a
        // weak_ptr). And at the program's end it may not be called at all (a line still going when the loop has
        // ended): the program closes every screen while its loop still runs (close()), and its driver's goodbyes
        // give back what is left.
        std::function<void()> done;
    };
    // Asks for a way in, and is answered once, now or later: a way, or why there is none ("no_picture" where
    // asking again does not change it soon). Never asked again while it has not answered. `wanted` says whether the
    // way is still waited for (the screen may have been closed or thrown away meanwhile): an asker that waits before
    // it signs in to the controller's web page asks it first, and asks nothing of the controller where it is no, nor
    // answers then. On this object's thread.
    using Answer = std::function<void(base::Result<WayIn>)>;
    using Wanted = std::function<bool()>;
    using Asker = std::function<void(Answer, Wanted)>;
    // One seat of a controller's console at a time, for every screen given this asker: it asks `asker` for a way in
    // only once the way it handed out last is done (its session's line has gone, WayIn::done), and has every other
    // asking wait for that, in turn; one that is no longer wanted when its turn comes is let go of, and nothing is
    // asked for it. A screen holds one seat at a time by itself; two screens of one controller (one thrown away while
    // its goodbye is slow, one made anew) hold one between them only by sharing a seat: this one's, or the program's
    // seat of that controller, whichever driver made them (ConsoleSeats). On one thread: the screens it is given to
    // live on the thread it was made on.
    static Asker oneSeatAtATime(Asker asker);
    // The screen, each time a frame changed it.
    using Shown = std::function<void(const bmc::aspeed::Picture&, std::span<const bmc::aspeed::Rect>)>;

    // What the console is held to, as the web's (redfish.py 141-205). A test makes the times short.
    struct Limits {
        int idleMs = 20000;  // without anybody looking, after which the console is given back
        int quietMs = 40000; // the controller has said nothing for this long: the connection is dead
        // After a session that came to nothing, before the next: longer every time.
        std::array<int, 4> retryMs = {30000, 60000, 120000, 300000};
        int giveUpMs = 600000;      // ...and after what asking again does not change
        int heldMs = 600000;        // ...and while somebody else is using the console
        int openingMs = 12000;      // for the line to be let in
        int settledMs = 60000;      // a session that ran this long and was ended has simply run its time
        int sayMs = 10000;          // what is said to the console is taken within this, or it has stopped listening
        int behindMs = 1000;        // "every frame so far is drawn", said this often while frames come
        int tickMs = 250;           // how often the times above are looked at
        int goodbyeWithinMs = 3000; // for the console's own goodbye, after this program's
        int letInAtMost = 8;        // times in one session that somebody asking for the console is let in
        int unreadable = 3;         // frames in a row that cannot be read: not a picture this program reads
        // Times a second the screen may change its size (or its detail of colour): a new size is a new picture of up
        // to 24 MiB. A server that starts changes its size a few times over seconds; one that does it more often
        // than this is not a screen being shown, and its console is left.
        int mostResizesASecond = 3;
        // Keys and the pointer (the web's redfish.py 161-177).
        int inputWaitMs = 12000;  // the longest keys wait for the console to be open and this program's
        int masterWaitMs = 3000;  // "the console is yours" comes within this of the pass being taken, or it is not
        int keysSettleMs = 5000;  // a first session's keyboard and mouse are attached to the server only after this
        int inputSayMs = 2000;    // nothing is said behind what has waited this long to leave
        int letGoWithinMs = 4000; // a key a typing left down is let go of within this
        int holdMs = 50;          // a combination is held this long
        int keyPauseMs = 4;       // between the keys of a text, and after each one is let go of
        int askAgainEvery = 25;   // keys of a text between two askings whether it may still be typed
        // The most that may wait in this program to go to the console (DeviceLine::Limits::mostUnsentBytes).
        qint64 mostWaitingBytes = qint64{64} * 1024;
    };

    ServerScreen(Asker asker, Shown shown, Limits limits, QObject* parent = nullptr);
    // Says goodbye to a session that is open, as close().
    ~ServerScreen() override;
    ServerScreen(const ServerScreen&) = delete;
    ServerScreen& operator=(const ServerScreen&) = delete;
    ServerScreen(ServerScreen&&) = delete;
    ServerScreen& operator=(ServerScreen&&) = delete;

    // The screen is wanted now (whoever shows it asks at least every Limits::idleMs). Opens a session where none
    // is open and none was refused for now; holds one open. What stands: ok once the screen has been shown in the
    // session that is open; "no_stream" while one opens or the screen has not come; why the last came to nothing,
    // until the next may be asked for.
    base::Result<void> look();
    // The session's screen as it stands; empty before its first frame and after it ended (the screen is not kept
    // beyond the line it came over).
    const bmc::aspeed::Picture& picture() const noexcept;
    // This program's session holds the console's keyboard and mouse: the controller said "the console is yours",
    // and has given it to nobody since.
    bool holdsTheConsole() const noexcept;
    // Frames of the session that is open taken off its line and not shown yet (read, or waiting to be): two at the
    // most, and whatever completes in the one message of the line under way as the second does.
    int framesWaiting() const noexcept;
    // The screen's still for a card `width` pixels wide (base/screen_still.h), made on the frames' thread from the
    // screen as it stands once the frames taken off the line before have been read, and handed to `done` later on this
    // thread: the packet, or why there is none ("no_stream": no session shows the screen now, or it ended first; what
    // the session came to where it ended for a reason). Not answered where this is thrown away first.
    using StillAnswer = std::function<void(base::Result<QByteArray>)>;
    void still(int width, StillAnswer done);
    // Ends the session that is open, letting go of what is down and saying goodbye, and opens none again. Whoever
    // waits for keys (askForInput) or a typing is answered, from inside this ("dropped"). Thrown away, the same, but
    // nobody is answered: whoever asked threw it away.
    void close();

    // The console's keyboard and mouse are asked for, to go in a turn (see above): a number no other turn of any
    // screen of this program has. Answered once, now or later,
    // within Limits::inputWaitMs: the turn, or why not: "in_use" where the console is somebody else's, or was given
    // to somebody at the controller's own page; "no_stream" where it is not open and this program's when the time
    // is up, or its keyboard is not attached yet; what look() says where the screen cannot be had now
    // ("console_held", "no_picture", ...). Asking looks at the screen (look()), and so keeps the console.
    using InputAnswer = std::function<void(base::Result<int>)>;
    void askForInput(InputAnswer answer);
    // The whole keyboard (the modifiers and the keys down), in that turn. Refused with why where the turn has
    // ended, and where the keyboard is not attached yet; "dropped" where what was said to the console before has
    // waited Limits::inputSayMs to leave (nothing that presses is said behind it; the console goes on), and where the
    // console has gone in the saying.
    base::Result<void> keyboard(const typed::Report& keys, int turn);
    // The mouse, as an absolute one: the buttons held (bits, typed::buttonBit), the place (0 to 32767 each way over
    // the screen) and a turn of the wheel. Refused as keyboard() is, and where the console says the server's mouse
    // is relative ("no_input": this program moves an absolute one only, so far), but for a report that lets go of
    // buttons, which goes as a relative one there.
    base::Result<void> mouse(std::uint8_t buttons, int x, int y, int wheel, int turn);
    // Whether keys of this turn may go: why not where the turn is over (as keyboard() would refuse them).
    base::Result<void> inTurn(int turn) const;

    // Typings: keys pressed together and let go of (an action's keys, Ctrl+Alt+Del), or a text typed, over the
    // console, one typing after the other (two at once would interleave on the one keyboard). Each asks for the
    // keyboard (askForInput), and asks `still` whether whoever asked may still have it done: before its first key,
    // and every Limits::askAgainEvery keys ("stopped" where not). Answered once, later: done, or why not: "invalid"
    // for keys or a text that cannot be typed (before anything is asked; the text never quoted), "stopped", or what
    // askForInput or keyboard() said.
    using Still = std::function<bool()>;
    using Typed = std::function<void(base::Result<void>)>;
    void press(const QString& keys, Still still, Typed done);
    void type(const QString& text, Still still, Typed done);
    // Every typing is stopped: the one under way where it is (a key it holds down is let go of), and those that
    // wait. Each is answered "stopped", from inside this.
    void stopTyping();

signals:
    // A session ended: how. An empty code: nobody looked any more, and it was given back.
    void ended(const QString& code, const QString& message);
    // A key or a button this program let go of may have stayed down on the server (its letting go did not leave in
    // time): said just before the session is ended for it (`ended`).
    void leftDown();

private:
    struct Session;
    struct Typing;
    struct StillAsked {
        std::uint64_t number = 0;
        StillAnswer done;
    };

    void open();
    void entered(WayIn way);
    void heard(const QByteArray& bytes);
    void take(const bmc::ami::Message& one);
    void sharing(const bmc::ami::Message& one);
    void video(const bmc::ami::Bytes& body);
    // What the frames' thread read and made since the last time (ScreenReader::take), taken.
    void takeReads();
    // A frame that could not be put together: one more in a row; the session is left after Limits::unreadable of them.
    void unreadable(const QString& why);
    // Whoever waits for a still is told why there is none.
    void stillsUnmade(const base::Error& why);
    // "Every frame so far has been drawn", where frames come and it was not said for a while.
    void behind();
    void tick();
    // Says one thing to the console; false where the session ended in the saying.
    bool say(std::span<const std::uint8_t> what);
    void lineEnded(DeviceLine::End why);
    // Lets go of the session's line: with a goodbye where it was greeted, kept by the program until it has gone.
    void leave();
    // The session is over, for this reason (an empty code: nobody looked); what follows from it for the next.
    void finish(const QString& code, const QString& message);
    qint64 now() const { return m_clock.elapsed(); }

    // Keys and the pointer.
    // Whoever asked for them and may be answered now is.
    void answerInput();
    // What an asking that waits until then is answered now; nothing while it waits.
    std::optional<base::Result<int>> inputNow(qint64 until, const base::Result<void>& looked) const;
    // Asks for the keyboard for a typing (by its number): taken out again where the typing ends first.
    void askForInput(InputAnswer answer, std::uint64_t typing);
    // Whether keys of this turn may go now; `letsGo`: the report only lets go, and goes behind what waits.
    base::Result<void> mayGo(int turn, bool letsGo);
    // A keyboard or mouse report of this turn, said.
    base::Result<void> sayInput(int turn, const bmc::ami::Bytes& report);
    // The console goes to somebody else: what this program has down there is let go of, and its turn ends. False
    // where the session ended in that.
    bool giveUpInput();
    // What this program has down on the server, let go of over the session's console. False where the session
    // ended in the saying.
    bool letGoOfWhatIsDown();
    // The turn is over, for this reason.
    void endTurn(base::Error why);
    // A letting go was said behind what waits: it has to leave within Limits::letGoWithinMs; and whether it has.
    void watchLettingGo();
    void letGoOwed();
    void inputTick();
    // Typings: the next step of the first, its end, and the queue.
    void queueTyping(std::unique_ptr<Typing> typing);
    void typingStep();
    void typingTurn(std::uint64_t number, base::Result<int> turn);
    void endTyping(base::Result<void> how);

    Asker m_asker;
    Shown m_shown;
    Limits m_limits;
    QElapsedTimer m_clock;
    QTimer m_ticks;
    std::unique_ptr<Session> m_session;
    // The session last given back, until its line has gone: the console's seat is not free before that, and no
    // other session is opened meanwhile.
    QPointer<QObject> m_leaving;
    // Where the frames are read (on a thread of their own), the screen as it was last handed back from there (empty
    // before a session's first frame, and after it ended), and the sessions so far, each the frames' by its number.
    std::shared_ptr<ScreenReader> m_reader;
    bmc::aspeed::Picture m_picture;
    std::uint64_t m_sessions = 0;
    // The stills asked for and not made yet, each by its number.
    std::vector<StillAsked> m_stillsAsked;
    std::uint64_t m_stillsMade = 0;
    bool m_asking = false; // a way in was asked for, and has not been answered
    QByteArray m_greeting; // what the session's console is answered with when it says it is allowed
    qint64 m_wantedAt = 0;
    bool m_closed = false;
    // What the last session came to, and when the next may be asked for.
    std::optional<base::Error> m_refusal;
    qint64 m_retryAt = 0;
    int m_failures = 0;
    std::optional<bool> m_completeFirst;
    // Keys and the pointer: the turn they go in now, and why the last one ended (what keys of an ended turn are told).
    int m_turn = 0;
    base::Error m_lost{"dropped", "The server's console was closed"};
    struct Asking {
        InputAnswer answer;
        qint64 until = 0;
        std::uint64_t typing = 0; // the typing it was asked for, if one
    };
    std::vector<Asking> m_askings;
    // A letting go that was said behind what waits to leave: by when it has to have left.
    std::optional<qint64> m_owed;
    QTimer m_inputTicks;
    // The typings, the first under way.
    std::deque<std::unique_ptr<Typing>> m_typings;
    std::uint64_t m_typingsMade = 0; // each typing's number
    QTimer m_typingSteps;
};

// The seat of each controller's console, for the whole program (review 3's MEDIUM 2, 2026-11-06): one seat at a time
// for every screen of a controller, whichever of its drivers made it. A driver is made anew whenever its card's
// address, user or password changes; its screen made a seat of its own, and while the old one's console said its
// goodbye, the new one signed in to the controller's web page a second time and opened a second session, which was
// told that every seat was taken, or asked the old one whether it might look and was told "console_held": somebody
// else at the console, who was this program.
//
// A controller is found as BoardHolds finds a KVM's hold: by its address as written, and by its own number (its
// Redfish service's, redfish::Marks::controller) where that is known, so that a card edited to another address of the
// same controller finds its seat there. Never by its certificate: its maker may have put one certificate on every
// board, and every server of a fleet would wait for one seat. Over its address alone, a controller at another address
// is another seat. Room is made of seats that nobody holds or waits for, once more than `most` are kept: what holds a
// seat is never let go of. Not across a restart of the program (its sessions do not outlive it either).
//
// On the interface thread, as the screens of the drivers that ask it.
class ConsoleSeats : public std::enable_shared_from_this<ConsoleSeats> {
public:
    static constexpr qsizetype mostKept = 4096;
    explicit ConsoleSeats(qsizetype most = mostKept);
    ~ConsoleSeats();
    ConsoleSeats(const ConsoleSeats&) = delete;
    ConsoleSeats& operator=(const ConsoleSeats&) = delete;
    ConsoleSeats(ConsoleSeats&&) = delete;
    ConsoleSeats& operator=(ConsoleSeats&&) = delete;

    // The program's own, for every driver that is given no other.
    static const std::shared_ptr<ConsoleSeats>& program();

    // What a controller is found by.
    struct Of {
        QUrl address;
        QString controller; // its own number, as its driver keeps it; empty: not known
    };
    // The asker for the screen of one driver: each time it is asked, it waits for the seat of the controller that
    // `of` says the driver speaks to then (none: the driver is gone, and it is answered "stopped" at once, no seat
    // taken), and asks `asker` for a way in once that seat is free, as oneSeatAtATime does.
    ServerScreen::Asker asker(std::function<std::optional<Of>()> of, ServerScreen::Asker asker);
    // Seats kept now.
    qsizetype kept() const { return m_kept.size(); }

    struct Seat;

private:
    // The seat of that controller, kept under both its keys from now on (one found under one of them is put under the
    // other too; two found, one under each, are one controller: the one taken, or waited for, stands).
    std::shared_ptr<Seat> seatOf(const Of& of);
    void makeRoom();

    qsizetype m_most;
    QHash<QString, std::shared_ptr<Seat>> m_kept;
};

} // namespace qkvm::core
