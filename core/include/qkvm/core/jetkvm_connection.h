#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

#include <qkvm/base/result.h>
#include <qkvm/core/jetkvm_kept.h>
#include <qkvm/core/strict_json.h>
#include <qkvm/core/webrtc_offer.h>

namespace qkvm::core {

class DeviceLine;
class JetKvmDevice;

// One WebRTC session to one JetKVM (M6 2f; design 2, plan 1.6): its signalling, the offer, JSON-RPC on
// `rpc` and the events it says there, and how long each may take, as the web's driver has them
// (jetkvm.py, _hold, _await_answer, _call, _on_message). The picture comes as the offering side hands it
// (WebRtcOffer), stamped as it became whole. It knows nothing of who watches: the viewer (2g) holds it.
//
// * The line is the driver's one road (JetKvmDevice::openSignalling): its session, signed in to once more
//   where the line refuses one somebody has since replaced.
// * The offer: {"type":"offer","data":{"sd": base64 of JSON {type, sdp}}}, video it only receives in the two
//   H.264 formats a JetKVM takes, and `rpc` (text, 64 KiB) and `hidrpc` (binary, 64 bytes) opened from here.
//   The answer, base64 of JSON whose `sdp` is text, within answerWithinMs, or `timeout`; {"error": ...} in
//   its place, or one that cannot be read, `bad_response`. Its addresses (`new-ice-candidate`) go to the
//   offering side as they come, their mid let go; `device-metadata`, `otaState` and the rest are let go.
//   The line falling after the answer ends nothing.
// * Up once `rpc` is open (`opened`); not within openWithinMs of start(): `timeout`.
// * JSON-RPC: numbers of its own from 1; an answer is taken only where its `id` is a whole number that waits,
//   in any order; callWithinMs each, or `timeout`; an `error` is `action_failed`, in its words made plain.
// * Events: `otherSessionConnected` (another viewer took it: `takenByAnother`, and nothing more of ours goes
//   to it; the stop word is read on the library's thread, before any drain), `videoInputState` (its input,
//   and the moment it came back, stamped as the word was read), `usbState`, `atxState`, `localVersion`.
// * Watched every watchEveryMs: it ends `dropped` where `rpc` closes under it, where no whole picture came for
//   frameStaleMs while it does not say "no signal", where its first came not within pictureAfterSignalMs of `rpc`'s
//   opening ("The JetKVM sent no picture", 10e), where none came within pictureAfterSignalMs after its
//   input came back (a picture counts as after that only afterSignalMarginMs or more after the word: the two
//   come over two of the library's callbacks, whose order can swap), or where the connection fails.
// * Its way in, step by step, in the video's diagnostics (VideoDiagnostics, numbers only): asked, the offer
//   sent, the answer, `rpc` open and the first picture, each with the ms since start().
//
// Keys and the mouse (rule 28 of the web's FOR-THE-DESKTOP-APP, part 3; jetkvm.py's _choose_road, _hid_report,
// _hid_keyboard, _keep_alive, _send_let_go, _let_go_at_hand_over, _settle_owed), as the JetKVM's own page sends
// them, over its binary `hidrpc` channel (jetkvm_keys.cpp):
// * The road: once `rpc` is open, `01 01` every shakeEveryMs on `hidrpc` until the JetKVM answers `01 01`, for
//   shakeWithinMs from that channel's opening (it is waited for as long). No answer, another version, or no channel:
//   JSON-RPC; then it is asked again every askAgainEveryMs, asksAgain times, and an answer then moves keys onto
//   `hidrpc` at the first moment nothing of the program's is down or on its way over JSON-RPC (the late road). No
//   key goes before the road is decided (keysReady), nor before what an earlier connection may have left down (the
//   JetKVM's JetKvmKept::Leftover) is let go of: `02 00`, `06 00 00 00` and `03` at the last place with no button.
// * The keyboard by keypress messages (`05 key 1|0`: those let go of first, then the modifiers, then those pressed),
//   `02 00` where nothing is down, and a keep-alive (`09`) every keepAliveEveryMs while a key that is no modifier is
//   held by one; the absolute mouse `03 x4 y4 b`, the relative `06 dx dy b`. The wheel goes as JSON-RPC
//   `wheelReport`, one at a time, each once the one before was answered, turns folded within a signed byte.
//   Macros (`07`, `08`) are never sent. A letting go on one mouse lets go of the same button on the other.
// * From the JetKVM: `33 m k1..k6`, and `keysDownState` on `rpc`, is its keyboard as it is (keyboardSaid).
// * At a hand-over (`otherSessionConnected`): one `02 00` where the keyboard said last had something down, and each
//   mouse's buttons up where it has one down, behind everything, and nothing else ever (the stop word stops the
//   rest); no keep-alive goes after it. Before this program ends a connection itself (letGo), the same letting go,
//   given letGoWaitMs to leave and letGoSettleMs to be done. A connection that ends with something down, or maybe
//   down, leaves it owed to the next one. The `hidrpc` channel closing ends the connection.
// * Where hidrpc does not answer (rule 24; jetkvm.py say, _say_in_turn, _call): every report as JSON-RPC, one at a
//   time, each once the one before was answered (a JetKVM does each call in a task of its own: sent together, keys
//   came out of order on a real one); places, steps and turns that wait are folded, a whole state the same as the last
//   of its kind that waits is not sent again. One not answered within takeWithinMs, or mostWaiting waiting, makes the
//   connection anew (madeAnew: the JetKVM lets go of every key as it ends). A press answered after the hand-over is
//   let go of at once; one not answered (its wait given up, or cut off) is let go of while the connection is open,
//   and, given up, again in turn as its answer comes after all (rule 25). Before this program closes a connection with
//   presses in the JetKVM's hands, and after a hand-over, the answers due are waited for handOverGraceMs, so that a
//   letting go comes after its press; what is cut off then is "may have been done".
//
// On the interface thread. Not deleted from inside its own signals (deleteLater is).
class JetKvmConnection : public QObject {
    Q_OBJECT

public:
    // The web's (jetkvm.py:90-111): CONNECT_TIMEOUT, RPC_TIMEOUT, FRAME_STALE, PICTURE_AFTER_SIGNAL; and for keys
    // and the mouse HID_SHAKE_EVERY, HID_SHAKE_WITHIN, HID_ASK_AGAIN_EVERY, HID_ASKS_AGAIN, KEEP_ALIVE_EVERY,
    // TAKE_WITHIN, MOST_WAITING, HAND_OVER_GRACE, LET_GO_WAIT, LET_GO_SETTLE and MOST_BUFFERED.
    struct Pace {
        int answerWithinMs = 15'000;
        int openWithinMs = 15'000;
        int callWithinMs = 8'000;
        int frameStaleMs = 5'000;
        int pictureAfterSignalMs = 20'000;
        int afterSignalMarginMs = 100;
        int watchEveryMs = 1'000;
        int shakeEveryMs = 250;
        int shakeWithinMs = 2'000;
        int askAgainEveryMs = 1'000;
        int asksAgain = 10;
        int keepAliveEveryMs = 30;
        int takeWithinMs = 5'000;
        int mostWaiting = 256;
        int handOverGraceMs = 2'000;
        int letGoWaitMs = 500;
        int letGoSettleMs = 50;
        qint64 mostBufferedBytes = 16'384;
    };
    // Which way keys and the mouse go over it.
    enum class Road { Undecided, Hid, Rpc };
    // A keyboard as a JetKVM's report says it: the modifiers as bits, up to six other keys (USB HID usages).
    struct Keyboard {
        int modifiers = 0;
        QList<int> keys;
        bool empty() const { return modifiers == 0 && keys.isEmpty(); }
        bool operator==(const Keyboard&) const = default;
    };
    // One of the live line's reports: the whole keyboard; the pointer at a place (0 to 32767 each way) or a step
    // (-127 to 127 each way), each with the buttons (bit 0 left, 1 right, 2 middle, 3 back, 4 forward); or a turn
    // of the wheel (-127 to 127 each way).
    struct Report {
        enum class Kind { Keyboard, Place, Step, Wheel };
        Kind kind = Kind::Keyboard;
        Keyboard keyboard;
        int x = 0;
        int y = 0;
        int buttons = 0;
    };
    // What it says of its input, clamped.
    struct Video {
        std::optional<bool> ready;
        int width = 0;
        int height = 0;
        double fps = 0;
        qint64 signalAtUs = 0; // where its input came back after it had said there was none
    };

    // `kept`: what the JetKVM keeps across its drivers (its Leftover); none, its own.
    JetKvmConnection(JetKvmDevice& device, quint64 serial, Pace pace, std::shared_ptr<JetKvmKept> kept = {},
                     QObject* parent = nullptr);
    ~JetKvmConnection() override;
    JetKvmConnection(const JetKvmConnection&) = delete;
    JetKvmConnection& operator=(const JetKvmConnection&) = delete;
    JetKvmConnection(JetKvmConnection&&) = delete;
    JetKvmConnection& operator=(JetKvmConnection&&) = delete;

    // Once.
    void start();
    // A question to it, answered a turn of the loop later at the soonest: its `result`, read by the book. Within
    // callWithinMs, or `withinMs` where that is given.
    void call(const QString& method, const QJsonObject& params, std::function<void(base::Result<json::Value>)> done,
              std::optional<int> withinMs = std::nullopt);
    void requestKey();
    void keyFramesOnly(bool only);
    void fromTheNextKeyFrame();

    bool isOpen() const { return m_open && !m_over; }
    bool taken() const { return m_taken; }
    quint64 serial() const { return m_serial; }
    const Video& video() const { return m_video; }
    // How many `videoInputState` events it said: an answer to getVideoState asked before one that came since is older
    // than it (10f).
    int videoEvents() const { return m_videoEvents; }
    std::optional<QString> usb() const { return m_usb; }
    std::optional<json::Value> atx() const { return m_atx; }
    QString version() const { return m_version; }
    // What the offering side refused of what came, counted.
    WebRtcOffer::Refused refused() const;
    // What its box handed on and let go of (WebRtcOffer::counts), as it stands.
    PictureBox::Counts counts() const { return m_peer ? m_peer->counts() : PictureBox::Counts{}; }
    // From here: the line closed, the peer closed, every call waiting told; `ended` is not said.
    void end();
    // Ended by this program, and kept until it is gone (as KvmdConsoleLine::letGo): nothing more of the live line's
    // goes; what it has down is let go of and given its time to leave (letGoWaitMs, letGoSettleMs); after a hand-over,
    // the answers still due are waited for (handOverGraceMs). Then end(), and deleted. Its signals are said to nobody
    // the caller connected (disconnect first). It ends with the program at the latest.
    static void letGo(std::unique_ptr<JetKvmConnection> connection);
    // The connections let go of that are letting go still, in the whole program.
    static int lettingGo();
    // At the program's end, while its loop still runs (main.cpp, once the drivers were let go of): the loop is run
    // until no connection is letting go any more, or `withinMs` has passed, so that what each had down is let go of
    // and given its time to leave before the program ends and closes them all at once (review 3's LOW 4: without a
    // server's goodbye to wait for, nothing ran the loop then, and the letting go went out in the same moment as the
    // close). At once where none is; not from inside a connection's own signals.
    static void finishLettingGo(int withinMs);

    // Keys and the mouse (above).
    Road road() const { return m_road; }
    // The road is decided and what was owed let go of: keys may go.
    bool keysReady() const { return m_keysReady; }
    // A report of the live line's, sent after everything said before it, without waiting (the reports of one
    // `holder`; `fold`: a place, a step or a turn, which may be folded into one of its kind that waits; `lettingGo`:
    // nothing down, which is never refused for its holder). An error where nothing goes: the connection is not up or
    // is ending ("dropped"), another viewer has the JetKVM ("in_use"), or keys are not ready on it yet.
    std::optional<base::Error> say(const Report& report, quint64 holder, bool fold = false, bool lettingGo = false);
    // What the JetKVM last said of its keyboard (`33`, or `keysDownState`), its keys in its own order.
    const std::optional<Keyboard>& keyboardSaid() const { return m_keyboardSaid; }
    // Whether this program ended it to make a new one (keys that could be done late and out of turn): the JetKVM is
    // not away for that.
    bool madeAnew() const { return !m_anew.isEmpty(); }
    // Ended to make a new one: what it has down let go of and given its time, then `ended` ("dropped").
    void makeAnew(const QString& why);
    // Keys go over it: up, not taken over, not ending; and why not, where they do not.
    bool live() const;
    base::Error endedError() const;

    // For an action that types (jetkvm_typing.h).
    // The live line's reports still on their way over JSON-RPC.
    bool keysOnTheirWay() const { return m_sending || !m_outbox.isEmpty(); }
    // An action types: keys do not change road meanwhile.
    void typing(bool on) { m_typing = on; }
    // Over hidrpc: the whole keyboard (`02`), and `heard` once the JetKVM says it has it (its `33`, the answer), or why
    // not (`in_use` where its own page took it first; what the end of the connection says where that comes first). An
    // error, and nothing heard, where it could not be sent.
    std::optional<base::Error> typeWhole(const Keyboard& keyboard,
                                         std::function<void(const std::optional<base::Error>&)> heard);
    // Over JSON-RPC: the whole keyboard as a keyboardReport, answered within `withinMs`; a press that is not answered
    // is let go of (rule 24).
    void press(const Keyboard& keyboard, int withinMs, std::function<void(base::Result<json::Value>)> done);
    // What this program has down over it, let go of now and not waited for (nothing after a hand-over).
    void letGoNow();
    // The keyboard layout the JetKVM says its computer uses (`getKeyboardLayout`), asked once a connection, as the
    // web's _layout takes it (jetkvm_layouts.h): US English where it has no such question (an older one: "Method not
    // found"). The asking's error where it does not answer: nothing is typed without knowing which keys type it.
    void keyboardLayout(std::function<void(base::Result<QString>)> done);

signals:
    void offerWent();
    void opened();
    void pictureCame(const QByteArray& annexB, bool key, qint64 arrivedUs);
    void videoSaid();
    void takenByAnother();
    void keysReadied();
    void ended(const QString& code, const QString& message);

private:
    struct Waiting {
        std::function<void(base::Result<json::Value>)> done;
        // A report of keys or the mouse (rule 24): what it says, and whether it presses something.
        bool report = false;
        bool mouse = false;
        std::optional<Report> press;
        qint64 id = 0;
    };
    // A whole keyboard said over hidrpc for an action, until the JetKVM says it has it.
    struct Echo {
        Keyboard keyboard;
        std::function<void(const std::optional<base::Error>&)> heard;
    };
    static constexpr qsizetype mostGivenUp = 32; // the web's MOST_GIVEN_UP
    // What waits to go over JSON-RPC for the live line.
    struct Queued {
        Report report;
        quint64 holder = 0;
        bool fold = false;
        bool lettingGo = false;
    };
    enum class Went { Nowhere, Hid, Rpc };
    static constexpr int rpcChannel = 0;
    static constexpr int hidChannel = 1;

    // A question sent, under the number it gives back; none where it was not sent (its asker told why).
    std::optional<qint64> ask(const QString& method, const QJsonObject& params, Waiting waiting, int withinMs);
    // A call is over (answered, or not: no answer in time, cut off, never sent), before whoever waits is told: what
    // its report said is settled there and then (jetkvm_keys.cpp).
    void settled(Waiting& call, bool answered);

    // jetkvm_keys.cpp.
    bool drained(Went went) const;
    // Over JSON-RPC, presses of the program's in the JetKVM's hands (sent and not answered, or given up on and not
    // answered yet): a letting go sent beside one could be done before it.
    bool pressesInHand() const;
    void letGoOfPressesInHand();
    void echoesFail(const base::Error& why);
    void hidOpened();
    void beginShaking();
    void shake();
    void decide(Road road);
    void settleOwed(std::function<void()> then);
    void askAgain();
    bool quiet() const;
    void heardHid(const QByteArray& bytes);
    void keysSaid(const Keyboard& said);
    std::optional<base::Error> sendHid(const QByteArray& bytes);
    std::optional<base::Error> hidReport(const Report& report);
    std::optional<base::Error> hidKeyboard(const Keyboard& now);
    void keepAlive();
    std::optional<Report> otherMouse(const Report& report) const;
    void queue(const Queued& queued);
    void sayInTurn();
    void sendReport(const QString& method, const QJsonObject& params, int withinMs, std::optional<Report> press,
                    std::function<void(base::Result<json::Value>)> done);
    void letGoOf(const Report& press);
    void noted(const Report& report);
    Went sendLetGo(bool keyboard = true);
    void letGoAtHandOver();
    void noMoreHid();
    // Waited for, every 10 ms, until `until` says so or `withinMs` is over; then `then`.
    void waitFor(std::function<bool()> until, int withinMs, std::function<void()> then);
    void closeAfterLettingGo(std::function<void()> then);
    void letGoAndWait(std::function<void()> then);
    void leftWhatWasDown();

    void lineCame(DeviceLine* line);
    void read(const QByteArray& text);
    void answered(const json::Value& data);
    void heard(const WebRtcOffer::Said& word);
    void watch();
    // A step of its way into the video's diagnostics (plan 4, for M1): `jetkvm_asked`, `_offer_sent`, `_answer`,
    // `_open` and `_first_picture` (with whether it was a key frame, and its size), each once, with the ms since
    // start(). Numbers only, as every record is.
    void step(const char* event, std::optional<qint64> key = std::nullopt,
              std::optional<qint64> bytes = std::nullopt) const;
    // Ends it and says why, once.
    void finish(const QString& code, const QString& message);
    void tell(std::function<void(base::Result<json::Value>)> done, base::Result<json::Value> what);

    QPointer<JetKvmDevice> m_device;
    quint64 m_serial = 0;
    Pace m_pace;
    std::shared_ptr<JetKvmKept> m_kept;
    QPointer<DeviceLine> m_line;
    std::unique_ptr<WebRtcOffer> m_peer;
    QHash<qint64, Waiting> m_waiting;
    QList<QByteArray> m_early; // addresses that came before the answer
    qint64 m_lastId = 0;
    QTimer m_watch;
    QTimer m_openWithin;
    QTimer m_answerWithin;
    Video m_video;
    std::optional<QString> m_usb;
    std::optional<json::Value> m_atx;
    QString m_version;
    qint64 m_askedUs = 0;  // when start() was called (steady)
    qint64 m_openedUs = 0; // when `rpc` opened (steady): its first picture is waited for from then
    int m_videoEvents = 0;
    bool m_started = false;
    bool m_answered = false;
    bool m_open = false;
    bool m_pictured = false; // its first picture came
    bool m_taken = false;
    bool m_over = false;
    bool m_closing = false; // this program ends it (letGo): nothing more of the live line's goes
    QString m_anew;         // why this program ends it to make a new one

    // Keys and the mouse (jetkvm_keys.cpp).
    Road m_road = Road::Undecided;
    bool m_keysReady = false;
    bool m_hidOpen = false;
    bool m_shaking = false;
    bool m_shaken = false;   // it answered `01 01`
    bool m_hidOther = false; // it answered another version
    int m_asksLeft = 0;      // the late road's askings
    QTimer m_shake;          // every shakeEveryMs while the road is chosen
    QTimer m_shakeWithin;    // the end of choosing
    QTimer m_hidWithin;      // the channel's opening, waited for
    QTimer m_lateRoad;       // asked again, and the quiet moment looked for
    QTimer m_keepAlive;
    qint64 m_keepAliveDueUs = 0;
    std::optional<Keyboard> m_keyboardSaid;
    // What this program last said of the keyboard and the mice over it, either way: what the JetKVM ends up with.
    Keyboard m_kbSent;
    bool m_placed = false; // the absolute mouse was put somewhere: m_absX, m_absY, with m_absButtons
    int m_absX = 0;
    int m_absY = 0;
    int m_absButtons = 0;
    int m_relButtons = 0;
    QList<int> m_held; // keys pressed by a keypress message and not let go of: kept alive
    bool m_hadDown = false;
    // The JSON-RPC road: what waits, whether a report is on its way, the mouse's on their way, and the calls of
    // reports not yet answered (also where nobody waits for them any more).
    QList<Queued> m_outbox;
    bool m_sending = false;
    int m_mouseCalls = 0;
    QSet<qint64> m_unanswered;
    // Presses sent over JSON-RPC and not yet answered, by their number; those answered after the JetKVM said its own
    // page took it; and those no answer came for in time (the newest few), should their answer come after all.
    QHash<qint64, Report> m_pressing;
    QSet<qint64> m_afterHandOver;
    QList<std::pair<qint64, Report>> m_givenUp;
    bool m_typing = false;
    QList<Echo> m_echoes;
    std::optional<QString> m_layout; // as it said it, once asked
};

} // namespace qkvm::core
