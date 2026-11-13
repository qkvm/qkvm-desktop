#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <qkvm/core/picture_box.h>

namespace qkvm::core {

// The offering side of one WebRTC session (M6 2e): an H.264 video it only receives, and data channels it
// opens itself, as a JetKVM takes a viewer (design 2: the device answers on channels the viewer opens).
// It knows nothing of a JetKVM: the formats, the channels and the word that stops it are handed in.
// Beside WebRtcReceiver, which answers a Janus offer; both stand on the same lock discipline
// (rtc_seam.h, M-D4) and the same mailbox of pictures (PictureBox). No STUN, no TURN: host addresses only.
//
// On the interface thread. The library's callbacks fill a mailbox under its lock and touch no QObject;
// the stop word is read on the library's thread, before that lock, and raises an atomic flag that
// whoever sends reads first (design 3.3).
class WebRtcOffer : public QObject {
    Q_OBJECT
public:
    // A data channel it opens: reliable and in order. Text or binary, and the largest message either way;
    // a message of the other kind, or larger, is dropped and counted.
    struct Channel {
        QByteArray label;
        bool text = true;
        qsizetype largest = 65536;
    };
    struct Setup {
        // The fmtp line of each H.264 format offered, in order: payload types 96, 97, and on.
        QList<QByteArray> h264Formats;
        QList<Channel> channels;
        // Whether a text message is the word that stops the session (another viewer took the device). On
        // the library's thread: pure, no lock, no QObject. Empty: nothing stops it.
        std::function<bool(QByteArrayView)> stopsOn;
        // The offer is said when the host addresses are gathered, or after this long with what there is.
        int gatherWithinMs = 2'000;
        // What may wait for the interface thread of what the channels said: beyond, it floods, and ends.
        int mostWaiting = 256;
        qint64 mostWaitingBytes = qint64{1} << 20;
        // Remote addresses taken at most: those said after the answer and those inside it alike.
        int mostCandidates = 40;
        // Whether an address of this PC's own loopback is taken: only where the far end's signalling address
        // is itself loopback (a test's stand-in). Otherwise a device could have this PC send to its own
        // services.
        bool loopbackAllowed = false;
        PictureBox::Pace pictures{8, 1'000};
    };
    // What a channel said, stamped as it was read on the library's thread.
    struct Said {
        int channel = -1;
        QByteArray bytes;
        qint64 arrivedUs = 0;
    };
    // What it refused or let go of what came, counted (each by its reason).
    struct Refused {
        quint64 wrongKind = 0; // text on a binary channel, or the other way
        quint64 tooLarge = 0;
        quint64 remoteChannels = 0;        // channels the far end opened, deleted
        quint64 candidates = 0;            // addresses not taken, for whichever reason of these:
        quint64 malformedCandidates = 0;   // no address of the kind ICE says (or a NUL, or too long)
        quint64 candidatesNotUdp = 0;      // TCP, or another transport
        quint64 namedCandidates = 0;       // a host by a name, which is never looked up
        quint64 unreachableCandidates = 0; // unspecified, multicast, the broadcast, or a loopback not allowed
        quint64 candidatesBeyond = 0;      // beyond the most taken
    };

    explicit WebRtcOffer(Setup setup, QObject* parent = nullptr);
    ~WebRtcOffer() override;
    WebRtcOffer(const WebRtcOffer&) = delete;
    WebRtcOffer& operator=(const WebRtcOffer&) = delete;
    WebRtcOffer(WebRtcOffer&&) = delete;
    WebRtcOffer& operator=(WebRtcOffer&&) = delete;

    // The peer, the receive-only track, the channels, and the offer being made: offerReady follows. Once:
    // begun again, closed or not, it refuses.
    bool begin();
    // The far end's answer: its video one of the formats offered (the first of them in its own line), with
    // one application, each under the mid it was offered under. False, and nothing taken, where it is no
    // such answer. Addresses inside it are taken out and go as candidate() takes them, after it.
    bool answer(const QByteArray& sdp);
    // One of the far end's addresses, as "candidate:" and the text after it, 2048 bytes at most, no NUL,
    // `mostCandidates` at most: UDP, to a host by its number (a name is never looked up), and not to one
    // that is unspecified, multicast, the broadcast, or this PC's loopback where that is not allowed. The
    // library is handed it made anew of what was judged, never its text (rtc_sdp.h, judgeAddress). Its
    // mid is the library's (the bundle's): the far end's is not trusted.
    bool candidate(const QByteArray& text);
    // A message on a channel, by the channel's kind; false where it is closed, stopped, of the other
    // kind (text with a NUL), or larger than the channel takes.
    bool send(int channel, const QByteArray& bytes);
    // The same, also after the stop word (never after close()): only for a letting go of what this side pressed,
    // which the far end still does over a session another took over from it (a JetKVM's hand-over, rule 28 of the
    // web's FOR-THE-DESKTOP-APP). Nothing else goes after the stop word.
    bool sendLettingGo(int channel, const QByteArray& bytes);
    // What a channel holds that has not left this PC yet, in bytes (0 where it is closed or there is none).
    qint64 bufferedAmount(int channel) const;
    // A key frame wished for: asked at the box's pace; one the pace refuses is asked when it has passed.
    // Nothing held is thrown away.
    void requestKey();
    void keyFramesOnly(bool only);
    void fromTheNextKeyFrame();
    // The stop word was read, or the session ended: raised on the library's thread.
    bool stopped() const;
    qint64 lastWholePictureUs() const;
    PictureBox::Counts counts() const;
    Refused refused() const;
    // Everything let go: the callbacks find nobody, the library's objects are deleted, no lock held.
    void close();

    // Test seam (M-D4), as WebRtcReceiver's: library calls made with a lock of these files held.
    static int libraryCallsUnderOwnLock();
    // Test seam (2e, the second reading): the far end's description as the library holds it, the addresses
    // it was handed among them, as it reads them. Empty before an answer.
    QByteArray remoteDescription() const;

signals:
    void offerReady(const QByteArray& sdp);
    void channelOpened(int channel);
    void channelClosed(int channel);
    void said(const qkvm::core::WebRtcOffer::Said& word);
    void pictureCame(const QByteArray& annexB, bool key, qint64 arrivedUs);
    void failed(const QString& reason);

private:
    struct Mailbox;
    void drain();
    void sayTheOffer();
    void askAWishThatWaits();

    Setup m_setup;
    std::shared_ptr<Mailbox> m_mailbox;
    int m_peer = -1;
    int m_track = -1;
    QList<int> m_channels;
    QList<int> m_offered;
    int m_candidates = 0;
    bool m_begun = false;
    bool m_offerSaid = false;
    QByteArray m_applicationMid; // as offered
    QTimer m_gather;
    QTimer m_wishWaits;
};

} // namespace qkvm::core
