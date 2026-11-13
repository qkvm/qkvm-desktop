#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <qkvm/engine/scene.h>

#include "d3d.h"
#include "decode_pool.h"
#include "decoder.h"
#include "gpu_core.h"

namespace qkvm::engine {

// How a picture's stored values become red, green and blue: the three rows the shaders are given.
// Each is multiplied with (luma, blue difference, red difference, 1), or with whatever else a
// picture's three planes hold.
struct ColourRows {
    float toRed[4];
    float toGreen[4];
    float toBlue[4];
};

// The rows for a picture, from what it says of itself and, where it says nothing, from what
// browsers assume: the small-screen standard for small pictures and for JPEG, the high-definition
// one for the rest.
ColourRows colourRowsFor(const AVFrame& frame) noexcept;

// What the scene draws for a stream: the planes to sample, which part of them is picture, and how
// to turn them into colours. `planes` is 0 while there is nothing to draw, 2 for a picture on the
// card (luma, and the two differences together), 3 for one decoded in software.
struct Drawable {
    int planes = 0;
    // Held, not named: what is drawn keeps its views (and with them their textures) for as long as it is
    // drawn, whatever lets go of them meanwhile (2c, the second reading).
    ComPtr<ID3D11ShaderResourceView> plane[3];
    SizePx picture;
    float cropRight = 1;
    float cropBottom = 1;
    ColourRows colours{};
};

// A stream, inside. Four kinds of state, each with its own lock and none held while another is
// asked for, except as GpuCore says:
//
//   the inbox      packets waiting to be decoded                    its own lock
//   the decoding   the decoder                                      its own lock, held while decoding
//   the slot       the newest decoded picture, not yet drawn        its own lock
//   the drawing    what is on the card for this stream              the device's lock
class StreamCore final : public Stream, public DecodePool::Work, public CardDecoderHolder {
public:
    StreamCore(GpuCore& gpu, const Host& host, Codec codec);
    ~StreamCore() override;

    void push(const Packet& packet) override;
    void shownAt(SizePx onScreen, Importance importance) override;
    bool takeKeyFrameWish() override;
    Stats stats() const override;

    void run() override;
    void dropCardDecoder() override;

    // The rest is for the scene, with the device's lock held; but hasNewer, which the decoding asks
    // as well, from its own thread.

    // Whether a picture newer than the one that is drawn is waiting.
    bool hasNewer() const;
    // Makes the waiting picture the one that is drawn; one decoded in software is copied to the card
    // for it. After `untilUs` a software picture is left waiting when there is an older one to draw
    // meanwhile, and true comes back: there is more to do at the next draw.
    bool takeNewer(std::int64_t untilUs);
    Drawable drawable() const;
    // The picture that is drawn has reached the screen at this time.
    void noteShown(std::int64_t atUs);
    // The stream was not drawn in a frame (it was not placed): the steps between its pictures on
    // the screen are counted afresh from the next one it is drawn with.
    void noteNotShown();
    // Lets go of everything this stream has on the card.
    void dropDrawObjects();
    // Shown large or controlled: its new pictures are drawn as soon as can be (Host::somethingUrgent),
    // and it is decoded before previews that wait (DecodePool). Any thread.
    bool urgent() const noexcept override
    {
        return !m_retired.load(std::memory_order_relaxed) &&
               m_importance.load(std::memory_order_relaxed) >= static_cast<int>(Importance::ShownLarge);
    }
    // Taken away from its scene: what waits goes, nothing more is decoded, and it is urgent no more
    // (2d's reading). Any thread; before the pool is handed it to end.
    void retire();

private:
    struct Waiting {
        std::vector<std::byte> bytes; // the packet, and the zeroes FFmpeg wants behind it
        std::size_t size = 0;
        std::int64_t arrivedUs = 0;
        bool keyFrame = false;
        // No packet of its own: the last picture decoded is decoded again (m_lastStill), as it is
        // when this is come to.
        bool again = false;
    };
    struct Plane {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        int width = 0;
        int height = 0;
    };
    struct CardViews {
        ID3D11Texture2D* texture = nullptr;
        UINT slice = 0;
        ComPtr<ID3D11ShaderResourceView> luma;
        ComPtr<ID3D11ShaderResourceView> colour;
    };

    // A stream of stills, of either kind (Codec::Still, Codec::KeyFrameStill).
    bool still() const noexcept { return m_codec == Codec::Still || m_codec == Codec::KeyFrameStill; }
    // A screen sent as what changed of it (Codec::Rgb).
    bool changes() const noexcept { return m_codec == Codec::Rgb; }
    // Applies one packet of changes to the screen as it stands (on the decoding's thread).
    void applyChanges(const Waiting& packet);
    // Copies to the card what changed of the screen since it was last copied (on the drawing thread).
    bool takeChanges(std::int64_t untilUs);
    // The bytes of the planes on the card now: what a whole picture copied to them is.
    std::uint64_t planeBytes() const noexcept;
    // A packet's buffer, kept to be used again, as many as the stream has use for and no more. Under the inbox's
    // lock.
    void keepSpare(std::vector<std::byte>&& bytes);
    // What waits was made from a screen that is lost now: nothing more is applied before a whole one.
    void waitForAWholeScreen();
    bool wantsTheCard() const;
    void chooseDecoder();
    void startAgain(bool tellTheHost);
    // Every hold of m_decodingLock ends here: it is let go, and then whether a packet waits is
    // looked at. One that came while it was held, whose wake found it taken and went away again,
    // would otherwise lie there until the next one came, and a KVM whose screen stands still sends
    // no next one. (A try_to_lock that fails is taken to mean that somebody holds it: Windows' own
    // locks, which std::mutex is here, do not fail otherwise.)
    void letGoOfDecoding(std::unique_lock<std::mutex>& decoding);
    void publish(Picture picture, double decodeMs, Decoding how);
    bool copyToTheCard(const AVFrame& frame);
    bool makePlanes(const AVFrame& frame, int count, int shiftX, int shiftY);
    bool drawFromTheCard(Picture& picture);

    GpuCore& m_gpu;
    const Host& m_host;
    const Codec m_codec;

    std::mutex m_inboxLock;
    std::deque<Waiting> m_inbox;
    std::vector<std::vector<std::byte>> m_spare;
    // A still stream's last picture that was decoded, as it came: decoded again where its picture
    // went with a lost device (a wall that is paused gets no next one), and what push() lets go of
    // where the same comes again. Written by run() alone, with the decoding held and under the inbox's
    // lock, and so always the newest decoded; read by run() with the decoding held and by push() under
    // the inbox's lock. As large as the picture, in a buffer as large as the largest it was used for: a
    // tile's JPEG is tens of kilobytes, a JetKVM's key frame some 130.
    Waiting m_lastStill;
    // A still stream's packet is being decoded (taken from the inbox, not yet kept or let go): one that
    // comes meanwhile is newer than whatever m_lastStill holds. Under the inbox's lock.
    bool m_stillInHand = false;
    // m_lastStill's picture could not be copied to the card, with the device as it was: it is not on
    // the screen, and the same still again is decoded again. Under the inbox's lock.
    bool m_lastStillUndrawn = false;
    // The copy just tried failed because a texture or a view could not be made. On the drawing thread.
    bool m_couldNotMake = false;
    // A screen's copy to the card failed (takeChanges): for how long the next try waits, which doubles with each such
    // failure in a row, up to a second; 0: the last copy did not fail. On the drawing thread. And a draw is to be
    // asked of the host for that try, when the pool wakes this at its time (run). Any thread.
    std::int64_t m_copyAgainInUs = 0;
    std::atomic<bool> m_drawForACopy{false};
    bool m_waitingForKeyFrame;

    std::mutex m_decodingLock;
    std::unique_ptr<Decoder> m_decoder;
    bool m_cardGaveUp = false;
    // The device's count of changes of decoding (GpuCore::decodingChanges), and of times it was
    // made anew (Gpu::timesRecovered), that this stream has seen.
    std::uint64_t m_decodingSeen;
    std::uint64_t m_recoveredSeen;

    mutable std::mutex m_slotLock;
    Picture m_newest;

    std::atomic<bool> m_keyFrameWish{false};
    std::atomic<bool> m_retired{false};
    std::atomic<int> m_importance{static_cast<int>(Importance::Preview)};

    mutable std::mutex m_statsLock;
    Stats m_stats;
    // When the last packet arrived and the last new picture was shown, for the steps between them,
    // and how long that picture took from arriving to the screen (below nought: not known).
    std::int64_t m_lastArrivedUs = 0;
    std::int64_t m_lastShownUs = 0;
    double m_lastTookMs = -1;

    // The drawing.
    Drawable m_drawn;
    FramePtr m_held; // a picture on the card is kept, so that the decoder does not write over it
    std::int64_t m_drawnArrivedUs = 0;
    // When the picture drawn began to be decoded, was decoded, and was taken to be drawn.
    std::int64_t m_drawnBeganUs = 0;
    std::int64_t m_drawnDecodedUs = 0;
    std::int64_t m_drawnTakenUs = 0;
    bool m_drawnIsNew = false;
    Plane m_planes[3];
    ComPtr<ID3D11ShaderResourceView> m_grey;
    AVPixelFormat m_planesFormat = AV_PIX_FMT_NONE;
    std::vector<CardViews> m_cardViews;

    // A screen sent as what changed of it (Codec::Rgb): the screen as it stands, in this program's memory, as three
    // planes of green, blue and red (as any picture with colours of its own is drawn), and what changed of it since it
    // was last copied to the card (all of it, where too much did, or where the card's copy went). Written by the
    // decoding and read by the drawing, under their own lock (GpuCore's list of locks): the decoding holds it while it
    // applies a packet, the drawing only tries it; neither asks for another lock while it holds it.
    std::mutex m_screenLock;
    FramePtr m_screen;
    std::vector<RectPx> m_dirty;
    bool m_dirtyAll = false;
    // ...and, under the slot's lock: what changed waits to be copied, and when the newest of it arrived, began to be
    // applied and was applied.
    bool m_changesWaiting = false;
    std::int64_t m_changesArrivedUs = 0;
    std::int64_t m_changesBeganUs = 0;
    std::int64_t m_changesDecodedUs = 0;
};

} // namespace qkvm::engine
