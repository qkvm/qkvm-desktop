#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <qkvm/base/result.h>
#include <qkvm/core/kvmd_console_line.h>
#include <qkvm/core/kvmd_device.h>
#include <qkvm/core/kvmd_stream.h>
#include <qkvm/core/kvmd_webrtc.h>
#include <qkvm/core/live.h>
#include <qkvm/core/own_device.h>
#include <qkvm/core/server_types.h>

namespace qkvm::core {

// A KVM of this PC's own that runs kvmd, live (live.h; SPEC.md section 6.3): its pictures as they
// come, the best way it has, and keyboard and mouse to it. One beside each KVM's driver, for as
// long as the driver lives. Whoever shows the KVM live is handed a Live of its own (`open`), and
// all of them share what is asked of the KVM: the driver gives one stream at a time, and two
// windows are not two loads on a KVM. While nobody has it live, nothing is asked of the KVM here.
//
// Pictures.
// * A native Janus/WebRTC receiver negotiates alongside the JPEG path below. Its first
//   complete H.264 key frame replaces JPEG reception for all viewers. Failure selects JPEG
//   and retries with a bounded backoff; unsupported offers and decoder failure wait longer.
//   Closing the last handle stops both paths. JPEG failure cannot cancel native negotiation.
// * First its stream of JPEGs (KvmdDevice::watch()). A stream has given when a picture of it was
//   handed on: a part that is no picture (it states no size, or one that is not shown) is not
//   one, and a stream that sends such parts and no picture for as long as a stream may take over
//   one picture (KvmdDevice::Pace::streamPictureMs) is ended here as one that sent none.
// * A stream that ended is asked for again after a wait, and after twice as long each time in a
//   row (`Pace::streamAgainMs`, up to `Pace::streamAgainLongestMs`): one that ended after its
//   pictures had come, one that answered with something that is no stream and says nothing of
//   whether there is one ("bad_response": an error of the KVM's own, or what calls itself a
//   stream of pictures and breaks the rules of one), and any that ended while the pictures
//   come one after the other. The waiting begins anew when a stream has given pictures for
//   as long as the longest wait. Meanwhile, and until the stream
//   that is asked for again has given a picture, its pictures are asked for one after the other,
//   so that the picture does not stand still. Only the first stream since somebody came, where
//   it could not be asked at all or broke off before a picture (the KVM is away, or refuses),
//   ends the live when no native attempt is still negotiating.
// * A KVM that says it serves no stream ("no_stream_here", before the first picture: nothing is
//   at the stream's place, or a yes comes from there with something of another kind) is asked
//   for its pictures one after the other instead, as it captured them (a width beyond its own
//   sizes, so that it shrinks nothing, which is what costs a KVM time). That it serves none is
//   remembered for a time (`Pace::noStreamForMs`), for whoever has it live and whoever comes:
//   then its stream is asked for again, and what it answers then holds. It is forgotten sooner
//   when the driver signs in anew (kvmd was started anew, and may be another by now), and the
//   stream is asked for a moment after that. A stream of which the driver says only that the
//   video rests ("no_stream") is not known to be missing: its pictures are asked for meanwhile,
//   which wakes the video, and the stream is asked for once more when the first of them has
//   come. Resting then again, with its video awake, it serves none. So a KVM whose gateway
//   answers for a stream it does not have costs two askings (kvmd_stream.h says why not one;
//   one where the driver has just had a picture of it, as a wall's cards take them), and a
//   stock KVM whose video rested has its stream at the second.
// * One after the other: the next when the last has come, never two at once; no more often than
//   whoever looks needs, and never more than `Pace::mostASecond`. Each is asked for as a new
//   one (KvmdDevice::askNewSnapshot): the driver answers a card's question that follows closely
//   on another with the picture it has, which would be five different pictures a second at the
//   most. "no_stream" (its video rests and is being woken, or it is slow) is asked again a
//   little later and is no failure. Three failures in a row end the live, with the driver's
//   code and sentence for the last. A picture that is byte for byte the one before it is handed
//   only to whoever has had none yet: a screen on which nothing changes gives the same picture
//   again.
// * Each picture is handed on as it came, a JPEG and a whole picture, and none is kept. Its size
//   is said (`videoChanged`) before the first, and whenever it is another; one that states no
//   size, or one that is not shown (kvmd::isShown), is not handed on.
// * What all who have it live need together is the widest and the fastest any of them asked for.
//
// Keyboard and mouse. `askControl(true)` has the driver open its line for them
// (KvmdDevice::openConsole), and `controlChanged(true)` says that the KVM has let it in. One holds
// them at a time: a second who asks is told no ("in_use"), without the KVM being asked anything.
// Every asking and every giving back is answered by one word of its own, in the order they were
// said, a turn of the loop later at the soonest: a giving back before they were given by two (a
// no for the asking, "given_up", and then the word for the giving back, which has no code and no
// sentence); a giving back by one who holds nothing by that one word. The exceptions: an asking
// by one who holds them already, or is being given them, gets no second word (one asking at a
// time is the caller's to keep); and nothing is said to one whose live is over.
// Where the driver hands out no line, its code and sentence are passed on: it could not sign in;
// or the KVM signs it in and refuses the line all the same ("not_allowed": told so by the driver
// when a line ends "auth_failed" under a pass the KVM has only just given, KvmdDevice::
// passWasRefused, and then for a while without the KVM being asked).
// Where the line ends by itself, its code and sentence are passed on (`controlChanged(false)`),
// and the pictures go on. Given back (`askControl(false)`), closed, thrown away, or the live
// over: everything that is down is let go first, by the line itself (kvmd_console_line.h). What
// is pressed reaches the line only from the one who holds it.
// Where the line could not let go of everything, whoever held keyboard and mouse is told that
// something may still be down on the machine (`leftDown`), whichever way they went: the line
// ended by itself and said so (KvmdConsoleLine::closed); or it took no more while it was let go
// of, on their being given back, on the live's end, or on `close()` (KvmdConsoleLine::close).
// Told just before the `controlChanged(false)` for the same end; after `close()`, for which none
// is said, a turn of the loop later. Whoever threw its Live away is not there to be told.
// And where it shows only later: the line said that all was let go, and then went without the
// KVM's own goodbye, which is the one sign that the KVM got what was said. It fell, or the KVM
// did not end it in its time and it was reset, the "up" that was said last with it. Whoever
// held keyboard and mouse is told `leftDown` then, late as that is (KvmdConsoleLine::letGo):
// after the `controlChanged(false)` for that end, by as long as the line was waited for, and
// only if that one is still there. Except for a giving back (`askControl(false)`): its
// `controlChanged(false)` is said once the line has gone, what it let go of landed by then (the
// KVM's goodbye came behind it), and so after that `leftDown`. Whatever goes to the machine
// next waits for that word, and does not land beside a key still on its way up (rule 27).
//
// What is said of the KVM's state (`stateChanged`) is what its driver says, when that is another
// than it was. A key frame is never asked for: every picture is whole.
//
// What a Live of this says after a call (a refusal, that keyboard and mouse were given back, the
// end of the live) it says a turn of the loop later, never from inside the call. `closed` is
// said for a live that ended by itself or was ended with `end`, and never after `close()`.
//
// On the interface thread. The driver outlives it, or `end` was called before the driver went.
class KvmdLive : public OwnLive {
    Q_OBJECT

public:
    // The times it keeps. A test makes them short.
    struct Pace {
        int mostASecond = 20;     // the most pictures a second that are asked for one after the other
        int restingAgainMs = 333; // its video rests and is being woken: how long until it is asked again
        int failedAgainMs = 1000; // after a failure: as long as the driver leaves a KVM alone at first
        // A stream that ended is asked for again after this long, and after twice as long each
        // time in a row, up to the longest. A second at first: as long as the driver leaves a KVM
        // alone after a failure, so that a stream which only fell is back at once to the eye.
        // Thirty at the most: as long as whoever shows a machine live waits at the most before it
        // asks anew (LivePicture), which is two askings a minute of a KVM that hangs up each
        // time. A stream that gave pictures for as long as that longest wait was a good one: the
        // waiting begins anew after it.
        int streamAgainMs = 1000;
        int streamAgainLongestMs = 30'000;
        // For how long it is remembered that the KVM serves no stream, before its stream is asked
        // for again: a minute. What stands before a KVM answers for a stream that is not there
        // while kvmd is started anew, for seconds; remembered for good, that left a KVM without
        // its stream until the program was ended. And a KVM that has none is asked one question
        // a minute for it while somebody has it live, which costs it nothing beside its pictures.
        int noStreamForMs = 60'000;
        int nativeAgainMs = 1000;
        int nativeAgainLongestMs = 30'000;
        int nativeUnsupportedMs = 60'000;
    };

    explicit KvmdLive(KvmdDevice& device, QObject* parent = nullptr);
    KvmdLive(KvmdDevice& device, Pace pace, QObject* parent = nullptr);
    ~KvmdLive() override;
    KvmdLive(const KvmdLive&) = delete;
    KvmdLive& operator=(const KvmdLive&) = delete;
    KvmdLive(KvmdLive&&) = delete;
    KvmdLive& operator=(KvmdLive&&) = delete;

    // The KVM live, for one who asks: the caller's from then on. It says nothing before the
    // caller holds it. Thrown away, it lets go as `close()` does.
    std::unique_ptr<Live> open(Live::Need need) override;
    // The KVM is taken away under whoever has it live (it leaves the wall, or its driver is
    // replaced): keyboard and mouse are let go there and then, and each is told `closed` with
    // this code and sentence a turn of the loop later. Nothing is asked of the driver after
    // this, and whoever asks for the KVM live all the same is told the same.
    void end(const base::Error& why) override;

    // How the machine's pointer is moved, as its KVM's mouse is set at the last look (KvmdDevice::mouseAbsolute):
    // said to whoever has it live (Live::pointerMode, pointerModeChanged), who sends the kind it takes.
    Live::Pointer pointer() const;

    // How many have it live just now, and what they need together.
    qsizetype watchers() const { return static_cast<qsizetype>(m_watchers.size()); }
    Live::Need need() const;
    // The KVM said that it has no stream of pictures, and that is still remembered: they are
    // asked for one after the other, and its stream is not asked for.
    bool servesNoStream() const { return m_noStreamUntil && m_clock.elapsed() < *m_noStreamUntil; }

private:
    // One asker's Live: what it is asked goes through here, and what is said comes out of it.
    class Handle;
    struct Watcher {
        Handle* handle = nullptr;
        Live::Need need;
        quint32 counter = 0;             // how many pictures it was handed
        std::optional<Live::Video> told; // the size it was told last
    };

    Watcher* watcherOf(const Handle* handle);
    QList<QPointer<Handle>> handles() const;
    void setNeed(const Handle* handle, Live::Need need);
    void leave(Handle* handle);
    void askControl(Handle* handle, bool on);
    bool holds(const Handle* handle) const;
    KvmdConsoleLine* lineOf(const Handle* handle) const;

    // The first is there: pictures are asked for. And nobody is any more: nothing is.
    void begin();
    void tryNative();
    void nativeEnded(const QString& code, const QString& reason);
    // Pictures every native attempt of this one's let go for want of room, the one now among them.
    quint64 letGo() const { return m_letGoBefore + (m_native ? m_native->picturesLetGo() : 0); }
    // `arrivedUs`: as the receiver stamped it, handed on as Live::Picture::arrivedUs.
    void handNative(const QByteArray& bytes, bool key, qint64 arrivedUs);
    void setTransport(const QString& transport);
    void startJpeg();
    void stop();
    void watch();
    void streamEnded(const QString& code, const QString& message);
    // The stream that was open is no more, for this reason: what is done about it.
    void streamIsOver(const QString& code, const QString& message);
    // Parts came, and no picture among them for as long as one may take: the stream is ended.
    void noPictureInTime();
    // The stream is asked for again after the wait, which is twice as long the next time; and
    // when that wait, or the time for which "no stream" is remembered, is over.
    void askForTheStreamLater();
    void askForTheStreamAgain();
    // The driver has signed in anew: what was remembered of the KVM's stream is forgotten.
    void driverSignedIn();
    void askOneAfterTheOther();
    void ask();
    void answered(const base::Result<Snapshot>& answer);
    // How far apart two pictures are asked for at the least.
    int betweenMs() const;
    void waitForTheNext();
    // Hands a picture to whoever has the KVM live. `mayBeTheOneBefore`: it came by asking, where
    // a screen on which nothing changed gives the picture it gave before. False: it is no
    // picture (it states no size, or one that is not shown), and nobody was handed anything.
    bool hand(const QByteArray& jpeg, bool mayBeTheOneBefore);
    // The live is over for everybody who has it, for this reason.
    void finish(const QString& code, const QString& message);
    void consoleMade(base::Result<std::unique_ptr<KvmdConsoleLine>> made);
    // Keyboard and mouse are nobody's from here on, and their line is let go. True: something
    // may still be down on the machine, which the line could not let go of any more.
    // `whenGone`: once that line has gone (KvmdConsoleLine::letGo), never from inside this call.
    bool giveBack(std::function<void()> whenGone = {});
    // `left`: something may still be down on the machine, which is said first, and also to one
    // that is not open any more.
    static void tellNoControlLater(Handle* handle, const QString& code, const QString& message, bool left = false);
    void tellState();

    std::unique_ptr<KvmdWebRtc> m_native;
    quint64 m_letGoBefore = 0;
    QTimer m_nativeAgain;
    int m_nativeWaitMs = 1000;
    bool m_nativeActive = false;
    qint64 m_nativeSince = 0;
    QString m_transport;
    QString m_fallback;
    QPointer<KvmdDevice> m_device;
    Pace m_pace;
    std::vector<Watcher> m_watchers;
    // Ended for good (`end`), and why.
    bool m_over = false;
    QString m_overCode;
    QString m_overMessage;

    // The stream, while one is open or asked for; whether it has handed on a picture, and when
    // its first and its last, by this one's clock; and the time a stream has to give a picture
    // once a part of it was none.
    std::unique_ptr<KvmdStream> m_stream;
    bool m_streamGave = false;
    qint64 m_streamGaveAt = 0;
    qint64 m_streamLastAt = 0;
    QTimer m_noPicture;
    // How long a stream that ends now waits to be asked for again; and the wait itself, which
    // is also how the time for which "no stream" is remembered comes to its end.
    int m_streamWaitMs;
    QTimer m_streamAgain;
    // The KVM serves no stream: remembered until then, by this one's clock. And, since a stream
    // was last asked for anew: it said that the video rests; and it was asked for once more
    // when a picture had shown the video awake.
    std::optional<qint64> m_noStreamUntil;
    bool m_rested = false;
    bool m_askedAwake = false;
    // Pictures are asked for one after the other; one such question is out; and how many of them
    // failed in a row.
    bool m_asking = false;
    bool m_out = false;
    int m_failures = 0;
    QElapsedTimer m_clock;
    qint64 m_askedAt = 0;
    QTimer m_next;
    bool m_pacing = false; // what `m_next` waits for is the pace, and not a KVM that rests or failed
    // What the last picture that came by asking was, as far as telling it from the next takes.
    std::optional<std::size_t> m_mark;
    qsizetype m_markedBytes = 0;

    // Who holds keyboard and mouse, or is being given them; the line for them; and a number
    // that is another whenever they are given back, so that a line which is made after that is
    // nobody's.
    Handle* m_holder = nullptr;
    std::unique_ptr<KvmdConsoleLine> m_console;
    quint64 m_controlTurn = 0;
    // The KVM has let the line in, and whoever holds them was told yes: before that they are
    // being given, and a giving back answers the asking as well.
    bool m_given = false;

    // What was last said of the KVM's state, and of how its pointer is moved.
    Live::Pointer m_pointer = Live::Pointer::Unknown;
    QString m_state;
    QString m_errorCode;
    QString m_errorMessage;
};

} // namespace qkvm::core
