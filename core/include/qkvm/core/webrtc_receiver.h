#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <qkvm/core/h264_rtp.h>

namespace qkvm::core {

// A single receive-only H.264 peer. Signaling belongs to the device adapter. C ABI keeps
// libdatachannel's release runtime separate from the application's debug/ASAN C++ objects.
// Network callbacks use a bounded mailbox, and never touch a QObject across threads.
//
// The far end's addresses (a Comet's Janus's, in its offer and trickled after it) are held to the one rule
// a JetKVM's are (rtc_sdp.h, judgeAddress): UDP to a host by its number (no name is ever looked up), one
// that may be reached (not unspecified, multicast, the broadcast, nor this PC's loopback unless the KVM is
// spoken to there), handed to the library made anew of what was judged, with the bundle's mid, and
// `mostCandidates` at the most; each refusal counted by its reason. Before, each went to the library as it
// came: a Janus could have had this PC look up names it chose, on a thread each, or send to its own
// services, other hosts' or a multicast group.
class WebRtcReceiver : public QObject {
    Q_OBJECT
public:
    struct Setup {
        // Remote addresses taken at most: those inside the offer and those trickled after it alike.
        int mostCandidates = 64;
        // Whether an address of this PC's own loopback is taken: only where the KVM's own address is
        // loopback (a test's stand-in).
        bool loopbackAllowed = false;
    };
    // What it refused of the far end's addresses, each by its reason (as WebRtcOffer::Refused counts them).
    struct Refused {
        quint64 candidates = 0;            // for whichever reason of these:
        quint64 malformedCandidates = 0;   // no address of the kind ICE says, a byte of another kind, too long
        quint64 candidatesNotUdp = 0;      // TCP, or another transport
        quint64 namedCandidates = 0;       // a host by a name, which is never looked up
        quint64 unreachableCandidates = 0; // unspecified, multicast, the broadcast, or a loopback not allowed
        quint64 candidatesBeyond = 0;      // beyond the most taken
    };
    explicit WebRtcReceiver(Setup setup, QObject* parent = nullptr);
    ~WebRtcReceiver() override;
    WebRtcReceiver(const WebRtcReceiver&) = delete;
    WebRtcReceiver& operator=(const WebRtcReceiver&) = delete;
    WebRtcReceiver(WebRtcReceiver&&) = delete;
    WebRtcReceiver& operator=(WebRtcReceiver&&) = delete;
    // The far end's offer: the description goes to the library with its addresses taken out of it, and those
    // go as candidate() takes them, after it.
    bool offer(const QByteArray& sdp);
    // One of the far end's addresses, as "candidate:" and the text after it: true where it was judged fit and
    // the library took it; false where it was refused (counted) or the library did not take it. Its mid is
    // the library's (the bundle's): the far end's is not trusted.
    bool candidate(const QByteArray& candidate);
    Refused refused() const { return m_refused; }
    void requestKey();
    // Pictures the mailbox let go for want of room (PictureBox::Counts::letGoBehind), as it stands; once
    // closed, as it was then.
    quint64 picturesLetGo() const;
    void close();
    // Protocol validation is independent of transport; useful to reject offers before ICE.
    static bool supports(const QByteArray& sdp);
    // Test seam (M-D4), read by tests only: how often this program called into libdatachannel while
    // holding a mailbox lock on the calling thread. The library calls the mailbox on its own thread
    // while holding its own lock, so such a call can leave both threads waiting for good: it must
    // stay 0.
    static int libraryCallsUnderOwnLock();
    // Test seam (the reading of 56258cd, L1), as WebRtcOffer's: the far end's description as the library holds
    // it, the addresses it was handed among them, as it reads them. Empty before an offer.
    QByteArray remoteDescription() const;

signals:
    void answerReady(const QByteArray& sdp);
    // `arrivedUs`: when it became whole, on this PC's steady clock, stamped on the library's thread
    // before it waited for this one (Live::Picture::arrivedUs).
    void pictureCame(const QByteArray& annexB, bool key, qint64 arrivedUs);
    void failed(const QString& reason);

private:
    struct Mailbox;
    void drain();
    // A window's wish the pace refused, asked when the pace has passed where no packet asked it.
    void askAWishThatWaits();
    std::shared_ptr<Mailbox> m_mailbox;
    QTimer m_wishWaits;
    Setup m_setup;
    Refused m_refused; // (on the interface thread only, as offer() and candidate() are)
    int m_peer = -1;
    int m_track = -1;
    int m_candidates = 0; // taken
    quint64 m_letGo = 0;
};
} // namespace qkvm::core
