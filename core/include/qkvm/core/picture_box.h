#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <qkvm/core/h264_rtp.h>

namespace qkvm::core {

// The pictures of a WebRTC mailbox (WebRtcReceiver's, WebRtcOffer's): its RTP packets put together into
// whole H.264 pictures, each stamped as it became whole, waiting for the interface thread to take them
// (design 3.2). Runs under its owner's mailbox lock, on the library's thread: it holds no lock of its
// own and calls nothing.
//
// It holds as many pictures as the engine lets wait (stream_core.cpp, h264PacketsWaiting): the drain
// hands all it holds over in one turn, and a stall of the interface thread shorter than that many
// pictures loses none. A picture beyond them keeps the run from the newest key frame among the ones
// held and itself, where that run is no longer than the most; one that is a key frame stands alone;
// otherwise all go, and a key frame is asked for (`lostAndAsks`); every picture let go is counted
// (`letGoBehind`), and its owner says so to whoever shows the machine (Live::picturesLetGo). A window
// that wishes for a key frame asks it here (`mayAsk`), and nothing is thrown away for that: on a
// shared connection the other windows go on.
class PictureBox {
public:
    struct Picture {
        QByteArray bytes; // Annex B, with SPS and PPS in front of a key frame (H264Rtp)
        bool key = false;
        qint64 arrivedUs = 0; // this PC's steady clock as it became whole
    };
    struct Pace {
        int most = 8;              // the engine's limit (stream_core.cpp)
        int keyAskEveryMs = 1'000; // a key frame asked for once in so long, at most
    };
    struct Counts {
        quint64 whole = 0;           // pictures put together, kept or not
        quint64 handed = 0;          // taken by the interface thread
        quint64 runsLost = 0;        // times all went because one more came than could wait
        quint64 letGoBehind = 0;     // every picture that went for want of room, of a run lost or not
        quint64 letGoAsCardOnly = 0; // no key frame, while only key frames are wanted
        quint64 keyAsks = 0;         // key frames asked for, for a loss or a window's wish
    };

    PictureBox(int payloadType, Pace pace);
    // The stream's SPS and PPS where its description gave them (H264Rtp::parameters).
    bool parameters(QByteArrayView sps, QByteArrayView pps);
    // One RTP packet as the library handed it over, at `nowUs`; one that is empty or larger than 64 KiB
    // is a loss. True where a picture waits to be taken.
    bool packet(QByteArrayView rtp, qint64 nowUs);
    // Only key frames are kept (a card, design 4: nobody shows the machine large); the rest is let go.
    void keyFramesOnly(bool only);
    // What is held goes, and from now on pictures are kept only from a key frame that comes after this
    // (F2: a window that opens gets every picture from the next key frame, never from one held before).
    void fromTheNextKeyFrame();
    // Whether a key frame is to be asked for now because one is needed here (a run was lost, none came
    // yet, pictures are kept only from the next one, or a window's wish was refused by the pace and no
    // key frame came since): true once in `keyAskEveryMs` at most. Asked at every packet.
    bool lostAndAsks(qint64 nowUs);
    // A window's wish for a key frame: true where one may be asked for now (once in `keyAskEveryMs` at
    // most, shared with `lostAndAsks`). Nothing held is thrown away. One the pace refuses is not
    // forgotten: `lostAndAsks` asks it once the pace has passed, unless a key frame came meanwhile.
    bool mayAsk(qint64 nowUs);
    // A wish that waits, asked where the pace allows it now (for a timer of its owner's, where no packet
    // comes to ask it at); false where none waits or the pace does not allow it yet.
    bool wishAsks(qint64 nowUs);
    // How long until the pace allows asking, in microseconds; 0 where it does now.
    qint64 paceLeftUs(qint64 nowUs) const;
    // All that waits, oldest first.
    QList<Picture> take();
    // When the newest whole picture became whole, kept or not; 0 before the first.
    qint64 lastWholeUs() const { return m_lastWholeUs; }
    Counts counts() const { return m_counts; }

private:
    bool asks(qint64 nowUs);
    void keep(Picture picture);

    H264Rtp m_assembler;
    Pace m_pace;
    QList<Picture> m_waiting;
    qint64 m_lastWholeUs = 0;
    qint64 m_askedAtUs = 0;
    bool m_asked = false;
    bool m_wished = false;
    bool m_cardOnly = false;
    bool m_untilNextKey = false;
    Counts m_counts;
};

} // namespace qkvm::core
