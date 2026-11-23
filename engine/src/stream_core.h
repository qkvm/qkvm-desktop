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

// A picture decoded in software made smaller, to `size` (a card's still, PERF-2), each of its planes by itself;
// nothing where it is of a kind the engine does not draw, or larger than the picture, or where there is no memory
// for it. What it says of its colours goes with it; where it says nothing of its standard, it says the one its own
// size had it read by (colourRowsFor).
FramePtr shrunkTo(const AVFrame& from, SizePx size);

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
    void shownAsACard(bool card) override;
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
    // Where its scene draws it now, the largest of its places, in the monitor's own pixels (Scene::place), with the
    // device's lock held. A still is kept at the size it is drawn at (keptSizeFor: PERF-2), and the last one is decoded
    // again where it is drawn larger than it was kept at. (A stream that is not drawn is told nothing, and keeps the
    // size it was last told; one never told is kept whole.)
    void placedAt(SizePx size);

private:
    struct Waiting {
        std::vector<std::byte> bytes; // the packet, and the zeroes FFmpeg wants behind it
        std::size_t size = 0;
        std::int64_t arrivedUs = 0;
        bool keyFrame = false;
        // No packet of its own: the last picture decoded is decoded again (m_lastStill), as it is
        // when this is come to.
        bool again = false;
        // ...because it is drawn larger than it was kept at (placedAt): the picture of it that waits to be drawn is
        // taken the place of, and that counts as no picture that made way.
        bool larger = false;
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
    // Stills that are key frames of H.264, each decoded by a decoder of its own (a JetKVM's card).
    bool keyFrameStills() const noexcept { return m_codec == Codec::KeyFrameStill; }
    // Those of a card of a wall (shownAsACard): handed in through the stills' line (StillsGate,
    // Gpu::Options::stillsAtOnce). Every other stream, a machine live or shown large among them, wakes the pool itself,
    // as before. (A card that comes to be shown large is out of the line from then on, in the step that makes it no
    // card: StillsGate::setCard; a place it holds is given back as it lets go, whatever it is by then:
    // letGoOfDecoding.)
    bool gated() const noexcept { return keyFrameStills() && m_member.card.load(std::memory_order_relaxed); }
    // It has something to decode: the pool is woken, or for a card's key frames the stills' line is handed it (`fresh`:
    // a still that came, and not the last one again; for what is said of the line).
    void handIn(bool fresh = false);
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
    // `size`: the stream's own (a still's before it was kept smaller); `remade`: the same still again, larger.
    void publish(Picture picture, double decodeMs, Decoding how, SizePx size, bool remade);
    // The size a still of this size is kept at to be drawn where it is placed now (PERF-2): the largest that fits its
    // place with its shape; its own where it is drawn at more than half of that (shown large), or where nothing is
    // known of its place. Any thread.
    SizePx keptSizeFor(SizePx picture) const noexcept;
    // The last still was kept smaller than it is to be drawn now, and nothing newer waits or is being decoded: it is
    // to be decoded again, and true (whoever asked wakes the pool for it). With `onceItHoldsStill`, only once its place
    // has not changed for a moment (a window's edge dragged: the review of f513c8e6, M3); before that nothing, and
    // `dueUs` says when to look again (whoever asked wakes the pool then, DecodePool::wakeAt). Under the inbox's lock.
    bool decodeAgainIfLarger(bool onceItHoldsStill, std::int64_t& dueUs);
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
    // m_lastStill's picture: its own size, and the size it was kept at to be drawn (PERF-2). Under the inbox's lock.
    SizePx m_lastStillWhole;
    SizePx m_lastStillKept;
    // Where the scene draws a still stream, the largest of its places, in the monitor's pixels (placedAt); 0: not told.
    std::atomic<int> m_placedWidth{0};
    std::atomic<int> m_placedHeight{0};
    // This stream as the stills' line knows it (a card's key frames are handed in there: gated), and whether it is a
    // card of a wall (shownAsACard, m_member.card, which the line changes under its lock): only a card's stills are
    // kept at the size drawn.
    StillsGate::Member m_member{*this};
    // When its place last changed (placedAt), by base::steadyNowUs.
    std::atomic<std::int64_t> m_placedAtUs{0};
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
