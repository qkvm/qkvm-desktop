#pragma once

#include <optional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <qkvm/base/result.h>
#include <qkvm/core/multipart_pictures.h>

class QNetworkReply;

namespace qkvm::core {

class KvmdDevice;

// A KVM's live picture as its stream of JPEGs (SPEC.md section 6.3): GET /streamer/stream, which
// answers with one picture after the other and never ends. KvmdDevice::watch() makes one, and
// nothing else does. It is the caller's from then on.
//
// On the interface thread, as the device is. It is the device's question: asked on the device's
// own lines and by its rules (the certificate looked at before a byte goes out, no proxy,
// followed nowhere), signed in first where there is no session, and signed in once more where
// the KVM has forgotten the one it had. It begins a turn of the loop after watch().
//
// * One stream to a device at a time. It holds one of the few lines Qt keeps to a place (six)
//   for as long as it is open, and the device's other questions share the rest: six streams
//   left none, and a look then never left this PC, with six pictures playing. What watch()
//   hands out while a stream of that device is open, or being opened, is a stream that asks the
//   KVM nothing and ends a turn of the loop later ("busy"). Several windows that show one KVM
//   live share one stream: KvmdLive (kvmd_live.h) asks for it and hands its pictures to each
//   of them. That is not done here.
// * Pictures are handed on as they come, and none is kept here. What is handed on are the bytes
//   of a part as the KVM sent them: whether they are a picture, the decoder says. Whoever is slow
//   to take one gets the next ones late: that is the receiver's to deal with (to show the newest
//   and drop the rest), and not done here.
// * What waits to be read is bounded (mostUnreadBytes, and one picture under way). A KVM that
//   sends faster than this PC reads is held back by the line, and is not kept in memory. And
//   one that sends pictures by the thousand a second, which no KVM has reason to, is not handed
//   on: the stream is over ("bad_response").
// * The stream has no end to wait for, so it has two limits of another kind, and is over by
//   either ("timeout"). One is on its silence: nothing at all comes for Pace::streamQuietMs,
//   from the question, from the beginning of the answer, or from the last bytes that came (or
//   from when somebody had taken the last picture, where that was later). The other is on each
//   picture: it is whole within Pace::streamPictureMs of the first byte that came behind the
//   picture before it. The second is the longer one, so that a large picture over a slow way
//   arrives, its bytes coming all the while; and it is there at all because a KVM that says a
//   picture of megabytes follows and sends a byte of it now and then is never silent, would
//   hold a line for ever, and whoever watches would keep the last picture up as if it were
//   live. The limit on silence takes a KVM to send something now and then also of a screen
//   that does not change: assumed of ustreamer as kvmd starts it, and not tried on a real KVM
//   yet.
// * ended() comes once, with why, in the driver's words:
//   - "unreachable", "timeout", "dropped", "cert_mismatch", "cert_invalid", "no_password",
//     "auth_failed" and "no_sign_in" as for every question; "dropped" also where the KVM ended
//     the stream, or
//     sent a part larger than is taken as a picture.
//   - "no_stream": its video rests and is being woken. kvmd says so itself, with 503 and in its
//     own shape. And what stands before kvmd says 502, 503 or 504 with a page of its own (a
//     gateway's: anything but kvmd's shape) while the program that makes the stream is not
//     running, which on a stock KVM is whenever nobody watches: taken for a video that rests
//     unless a picture of the KVM has just come. Asking again soon is the thing to do.
//   - "no_stream_here": this KVM has no such stream. It answers that there is no such place
//     (404). Or a gateway answers for it as above although its video is awake: a picture of
//     it has come within the last moments (Pace::freshMs: as long as what is known of a KVM
//     counts as how it is). Whoever asked takes its pictures one at a time
//     (KvmdDevice::askNewSnapshot), and need not ask this device for a stream again. (A
//     gateway's page alone does not tell the two apart. So of a KVM nothing was seen of, the
//     first such answer is "no_stream", and the one that follows a picture is this.) Or it
//     answers with a yes (200) and something of another kind than a stream of pictures: what
//     it names is not multipart/x-mixed-replace. One maker's KVM, whose live picture comes
//     another way, was seen to do both (a real one): the gateway's page while its video
//     rests, and the yes with another thing that lives at this place while it is awake.
//     Which of the two a 502, 503 or 504 is, what comes with it says. That is waited for as
//     long as a stream may be silent, counted from the beginning of the answer (and not anew
//     from each byte of it), and then read as far as it came. Whatever becomes of it
//     (it comes slowly, the line falls under it, it is packed and so not read), a KVM that
//     said no has answered: it does not count as away for that.
//   - "not_allowed": the KVM refuses its stream under a session it has only just given. The
//     stream is what it refuses, and not the session, which the driver keeps: every other
//     question is served under it. It is not asked for again for a while (Pace::signInRetryMs).
//   - "busy": a stream of this device is open already (above).
//   - "bad_response": it answered with something else (another number than those above), or
//     with what calls itself a stream of pictures and is not one this program reads: it names
//     no word between its parts, or breaks a stream's rules later. That says nothing of
//     whether the KVM has a stream.
//   - "closed": the device is gone.
//   A stream that is over is never opened again: whoever wants on asks the device for another.
// * After close() it says nothing more, and neither does one that was let go of.
// * It says what it says from the middle of the line's work. Whoever hears it may close it, or
//   let go of it, there and then; the device, as ever, is ended a turn of the loop later.
// * While it is open, and at the moment it ends, its device counts the KVM's picture as wanted:
//   the video is kept awake for as long, and Pace::restAfterMs beyond its end.
class KvmdStream : public QObject {
    Q_OBJECT

public:
    // What only a KvmdDevice has to give: nobody else makes a stream.
    class Made {
        friend class KvmdDevice;
        Made() = default;
    };
    KvmdStream(Made made, KvmdDevice* device);
    ~KvmdStream() override;
    KvmdStream(const KvmdStream&) = delete;
    KvmdStream& operator=(const KvmdStream&) = delete;
    KvmdStream(KvmdStream&&) = delete;
    KvmdStream& operator=(KvmdStream&&) = delete;

    // Ends it without a word: the line is given up, and nothing is said from here on.
    void close();
    // Whether it has ended, by itself or by close().
    bool isOver() const { return m_over; }

    // The most of the stream that waits here to be read. While this thread is busy elsewhere
    // nothing beyond it is taken off the line, and a line nothing is taken off holds the KVM back:
    // at thirty pictures a second that is a few of them.
    static constexpr qint64 mostUnreadBytes = qint64{1024} * 1024;
    // How many bytes of the stream it holds at this moment: what has come and waits to be read,
    // and the room made for the picture under way. (For the tests, which look that this stays
    // bounded while whoever is handed the pictures is slow.)
    qint64 held() const;

signals:
    // One picture of the stream, as the KVM sent it.
    void pictureCame(const QByteArray& jpeg);
    // Once, unless it was closed first: why it is over.
    void ended(const QString& code, const QString& message);

private:
    friend class KvmdDevice;

    void open();
    void ask(const base::Result<QByteArray>& token);
    void answered();
    // What a refusal means by what came with it: kvmd's own words, or somebody else's.
    base::Error refusedIn(int status, const QByteArray& words);
    // And what somebody else's means: that the video rests, or that there is no stream here.
    base::Error saidByAGateway();
    void read();
    void take();
    void finished();
    // Nothing has come for too long; and the picture under way has taken too long to be whole.
    void quiet();
    void overdue();
    void end(const base::Error& why);
    void letGo();
    void dropQuestion();
    // The device is being ended: nothing of it is touched from here on.
    void deviceGone();

    KvmdDevice* m_device; // null once the stream is over: only a stream that is not is known to its device
    QPointer<QNetworkReply> m_reply;
    QByteArray m_used;       // the session the question went out with
    bool m_relogged = false; // signed in anew after the KVM refused the session
    bool m_open = false;     // the KVM has answered with a stream of pictures
    int m_refusedWith = 0;   // or with 502, 503 or 504: what comes with that is waited for
    bool m_over = false;
    bool m_closed = false;  // by close(): nothing is said any more
    bool m_reading = false; // a picture is being handed on right now
    std::optional<MultipartPictures> m_pictures;
    QTimer m_quiet;
    // The time the picture under way has to become whole; and whether bytes have come since
    // the last whole one, which is when that time begins.
    QTimer m_pictureDue;
    bool m_pictureBegun = false;
    QElapsedTimer m_clock;
    qint64 m_second = 0; // the second that is being counted in, and the pictures that came in it
    int m_cameInIt = 0;
};

} // namespace qkvm::core
