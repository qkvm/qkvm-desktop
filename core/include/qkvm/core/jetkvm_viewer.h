#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <qkvm/base/result.h>
#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/jetkvm_kept.h>
#include <qkvm/core/server_types.h>

namespace qkvm::core {

class JetKvmDevice;

// A JetKVM's one viewer, as this program holds it (M6 2g; design 3.4, 3.6 and 4; plan 1.8): the one
// connection (JetKvmConnection) for its cards and every window that shows it, in its driver. On the
// interface thread.
//
// * One connection at a time for the KVM, across its drivers (JetKvmKept): a new one is begun only once
//   the one before is gone, waited for endingWaitMs at most; after that the asking fails with what the one
//   before ended in, or `not_connected` ("The JetKVM is taking too long to connect"). A connection that
//   ended by itself is begun again retryPauseMs later; one that found it away (unreachable, timeout, dropped,
//   a certificate not its own) is an attempt that found it so, and nothing connects until the driver's growing
//   hold is over (JetKvmDevice::heldBack, which a card is told meanwhile; the web's _check_backoff). "Show
//   here" is tried at once, whatever hold stands.
// * Cards (stillForACard): a card that asks is somebody watching, and connects where nothing does. It is
//   answered, in this order: `in_use` while the program stands aside; `not_through`, where no connection is
//   open, after an offer that came to nothing (below), and it connects nothing; the newest key frame where it is at
//   most frameStaleMs old and arrived afterSignalMarginMs or more after the JetKVM said its input came
//   back; with no signal, the newest key frame (or nothing) with signal false; otherwise `no_stream`,
//   "Connecting to the JetKVM" or "Waiting for the JetKVM's video".
// * Windows: while none shows it, its connection keeps key frames only; the first that comes has every
//   picture from the next key frame that arrives after that moment; a key frame asked loses nothing.
// * UNWATCHED: every unwatchedEveryMs, the connection is given back where no window shows the JetKVM and
//   no card asked for unwatchedMs. A look never counts as watching.
// * Looks (askStatus): asked over the connection where one is open, never connecting.
// * Stepping aside: another viewer's word over the newest connection (or as JetKvmKept::judge says) sets
//   the KVM aside, ends the connection, and says so once (steppedAside). While aside nothing connects:
//   not a card, a look, a window or a retry; only resume().
// * An offer that came to nothing (from the second reading of c7ec58c, B): each offer takes the JetKVM's one
//   viewer from whoever has it, and where this PC's video never comes through (UDP blocked, ICE failing) the
//   program never hears that other viewer, and never steps aside. So once an offer went and its connection
//   ended without coming about, cards make no offer (they are told `not_through`, and a look says it on
//   the card, with "Show here"); only a window that is opened, and resume(), offer again. A window that
//   stays open retries at the driver's growing hold, as before: the person is watching it, and asked for it.
//   A connection that comes about ends that.
// * Another JetKVM at its address (J5: its driver asks which JetKVM answers before every line, JetKvmDevice): no line
//   and no offer; every card is told so (`another_machine`) until the one it is known by answers again, and a card's
//   connection asks again at the driver's sameJetKvmEveryMs; "Show here" and a window that is opened ask at once, and
//   every window is told so for good (anotherAnswers).
class JetKvmViewer : public QObject {
    Q_OBJECT

public:
    // What a card is told, and a look says, after an offer that came to nothing.
    static constexpr const char* notThroughCode = "not_through";
    static constexpr const char* notThroughSentence =
        "The JetKVM answers, but its video does not come through to this PC. Show here tries again.";

    struct Pace {
        JetKvmConnection::Pace connection;
        int unwatchedMs = 300'000;     // the web's UNWATCHED
        int unwatchedEveryMs = 10'000; // how often that is looked at
        int endingWaitMs = 8'000;      // the web's HAND_OVER_GRACE and 6 s
        int retryPauseMs = 3'000;      // the web's RETRY_PAUSE
        // How long keyboard and mouse wait for a connection whose keys are ready: the web's _keys_ready (twice the
        // handshake's window and four TAKE_WITHIN), and the connecting before it (CONNECT_TIMEOUT).
        int inputReadyWithinMs = 39'000;
    };

    JetKvmViewer(JetKvmDevice& device, std::shared_ptr<JetKvmKept> kept, Pace pace, QObject* parent = nullptr);
    ~JetKvmViewer() override;
    JetKvmViewer(const JetKvmViewer&) = delete;
    JetKvmViewer& operator=(const JetKvmViewer&) = delete;
    JetKvmViewer(JetKvmViewer&&) = delete;
    JetKvmViewer& operator=(JetKvmViewer&&) = delete;

    base::Result<Snapshot> stillForACard();
    void windowCame();
    void windowLeft();
    void askForAKeyFrame();
    // A key of an action is about to go over its connection: it is in use, whoever looks or does not (the web's
    // _wanted_at before every key), and is not given back for nobody watching meanwhile (review 3's LOW 3).
    void keyGoes();
    // How it is, over its connection where one is open (false, and nothing asked, where none is). Nothing
    // where the connection went meanwhile, given back by this program: nothing was heard of the JetKVM then.
    bool askStatus(std::function<void(const std::optional<Status>&)> done);
    // "Show here": the program no longer stands aside, and connects at once. Done once open; or what the
    // connecting ended in.
    void resume(std::function<void(base::Result<Done>)> done);
    bool aside() const { return m_kept->aside; }
    // An offer came to nothing, and no card makes another (above).
    bool notThrough() const { return m_kept->notThrough; }
    bool isOpen() const;
    // Keyboard and mouse can be sent: the connection is up and its keys ready (JetKvmConnection::keysReady), or is
    // made now, as for "Show here" (but not where the program stands aside: `in_use`). The connection they are held
    // on, a turn of the loop later at the soonest: keys pressed over one are let go of by the JetKVM as it ends, and
    // whoever held them then holds nothing on the next. Or why not, within inputReadyWithinMs.
    void inputReady(std::function<void(base::Result<QPointer<JetKvmConnection>>)> done);
    // The same for what is asked over its connection and needs no keys (power, a restart, Wake-on-LAN, the jiggler):
    // once it is open.
    void connected(std::function<void(base::Result<QPointer<JetKvmConnection>>)> done);
    // What its connection's box handed on and let go of, as it stands (none while no connection is).
    PictureBox::Counts counts() const;
    // What the last connection showed of its input (false: no signal), until another shows otherwise.
    std::optional<bool> lastSignal() const { return m_told; }
    void forgetWhatWasShown() { m_told.reset(); }

signals:
    void pictureCame(const QByteArray& annexB, bool key, qint64 arrivedUs);
    void steppedAside();
    // A connection found another JetKVM at its address (J5, "another_machine", in these words): nothing was offered.
    void anotherAnswers(const QString& message);
    // The connection of this serial is over, ended by itself or given back: whatever was held on it went with it.
    void connectionOver(quint64 serial);

private:
    // What its four questions of a look were answered: its video, its USB, its extension, its version.
    using Said = std::array<std::optional<base::Result<json::Value>>, 4>;

    qint64 now() const { return m_clock.elapsed(); }
    // `eventsAtAsking`: what the connection had said of its input in events when the look was asked; an answer to
    // getVideoState asked before an event that came since is older than it, and the event's word stands (10f).
    std::optional<Status> made(const Said& said, const std::optional<base::Result<json::Value>>& atx,
                               const QPointer<JetKvmConnection>& askedOver, int eventsAtAsking);
    void want();
    void begin();
    void ended(quint64 serial, const QString& code, const QString& message);
    void taken(quint64 serial);
    // The connection given back by this program (UNWATCHED, aside, the driver gone): no `ended` is said.
    void letGo();
    void tick();
    void tellResumed(const base::Result<Done>& what);
    static void tellOne(std::function<void(base::Result<Done>)> done, base::Result<Done> what);
    // Whoever waits for keys (`keys`), or for an open connection, is told: the connection `on`, or why not.
    struct Asking {
        bool keys = true;
        std::function<void(base::Result<QPointer<JetKvmConnection>>)> done;
    };
    void asked(bool keys, std::function<void(base::Result<QPointer<JetKvmConnection>>)> done);
    void tellInputReady(JetKvmConnection* on, const std::optional<base::Error>& why, bool keys = true);

    QPointer<JetKvmDevice> m_device;
    std::shared_ptr<JetKvmKept> m_kept;
    Pace m_pace;
    std::unique_ptr<JetKvmConnection> m_connection;
    bool m_cameAbout = false; // that connection opened
    int m_windows = 0;
    QElapsedTimer m_clock;
    qint64 m_wantedAt = 0;
    QByteArray m_newestKey;
    qint64 m_newestKeyUs = 0;
    std::optional<base::Error> m_lastError;
    qint64 m_retryAt = 0;
    QTimer m_tick;
    QTimer m_retry;
    QTimer m_endingWait;
    QMetaObject::Connection m_waitingForEnding;
    std::optional<bool> m_told;
    QList<std::function<void(base::Result<Done>)>> m_resuming;
    // Whether the program stood aside when the first of those waiting pressed "Show here": where that does not come
    // to it, it stands aside again (from the third reading of the twins, L3). The same of an offer that had come to
    // nothing: where "Show here" does not come to it, cards make no offer again.
    bool m_asideBeforeShowHere = false;
    bool m_notThroughBeforeShowHere = false;
    QList<Asking> m_inputWaiting;
    QTimer m_inputWithin;
    bool m_going = false; // being destroyed: nothing is said
};

} // namespace qkvm::core
