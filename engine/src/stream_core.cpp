#include "stream_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <thread>

#include <qkvm/base/clock.h>
#include <qkvm/base/screen_changes.h>

namespace qkvm::engine {

namespace {

// How many packets may wait for the decoder. A burst from the network fits. A decoder that is
// further behind than this is not made to work through pictures nobody will see: it is brought
// forward to the next key frame. A JPEG stands by itself, so only the newest one waits.
constexpr std::size_t h264PacketsWaiting = 8;
constexpr std::size_t stillsWaiting = 1;
// A screen's changes are applied as they come, which takes moments; more waiting than this is a screen that changes
// faster than it is applied (megabytes each), and it is brought forward to a whole screen.
constexpr std::size_t changesWaiting = 4;
// What changed of a screen is copied to the card a rectangle at a time; more rectangles than this waiting to be
// copied are copied as the whole screen.
constexpr std::size_t mostRectsToCopy = 64;
// A screen whose copy to the card failed (a texture not made: the card's memory full) is tried again at a draw asked of
// the host this long after the failure, and twice as long after each failure in a row, up to a second: a card that
// stays full is tried once a second, not at every frame.
constexpr std::int64_t firstCopyAgainUs = 50'000;
constexpr std::int64_t longestCopyAgainUs = 1'000'000;

// A mean that follows what is measured, weighing about the last ten measurements.
double towards(double mean, double value) noexcept
{
    return mean == 0 ? value : mean + (value - mean) * 0.1;
}

// The place of a time among steps: the first step it is not over, or the last place for what is slower.
template <std::size_t Steps>
std::size_t placeAmong(const std::array<double, Steps>& steps, double ms) noexcept
{
    std::size_t place = 0;
    while (place < steps.size() && ms > steps[place]) {
        ++place;
    }
    return place;
}

// The numbers of a colour standard: how much of each difference goes into each colour.
struct Standard {
    float redFromRed;
    float greenFromBlue;
    float greenFromRed;
    float blueFromBlue;
};
constexpr Standard smallScreen{1.402F, 0.344136F, 0.714136F, 1.772F};  // ITU-R BT.601
constexpr Standard highDefinition{1.5748F, 0.1873F, 0.4681F, 1.8556F}; // ITU-R BT.709

// The formats in which FFmpeg says "the whole range of values is used" by the format's name.
bool usesTheWholeRange(int format) noexcept
{
    return format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P ||
           format == AV_PIX_FMT_YUVJ440P;
}

// How a picture decoded in software lies in memory, if it is a kind the engine draws: eight bits a
// value, one plane of luma, and either no colour or two planes of it, each smaller by these shifts.
struct Layout {
    int planes = 0;
    int shiftX = 0;
    int shiftY = 0;
};

Layout layoutOf(int format) noexcept
{
    // Green, blue and red, a plane each: what a picture with colours of its own is made into.
    if (format == AV_PIX_FMT_GBRP) {
        return {3, 0, 0};
    }
    const AVPixFmtDescriptor* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    const std::uint64_t unwanted = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM |
                                   AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_BE;
    if (description == nullptr || (description->flags & unwanted) != 0 || description->comp[0].depth != 8) {
        return {};
    }
    if (description->nb_components == 1) {
        return {1, 0, 0};
    }
    if (description->nb_components == 3 && (description->flags & AV_PIX_FMT_FLAG_PLANAR) != 0 &&
        description->comp[1].plane == 1 && description->comp[2].plane == 2 && description->comp[1].step == 1) {
        return {3, description->log2_chroma_w, description->log2_chroma_h};
    }
    return {};
}

int shifted(int value, int shift) noexcept
{
    return (value + (1 << shift) - 1) >> shift;
}

} // namespace

ColourRows colourRowsFor(const AVFrame& frame) noexcept
{
    if (frame.format == AV_PIX_FMT_GBRP) {
        // Nothing to work out: the planes are green, blue and red themselves.
        return {{0.0F, 0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}};
    }
    const bool whole = frame.color_range == AVCOL_RANGE_JPEG || usesTheWholeRange(frame.format);
    bool high = false;
    switch (frame.colorspace) {
    case AVCOL_SPC_BT709:
        high = true;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        high = false;
        break;
    default:
        high = frame.height > 576 && !usesTheWholeRange(frame.format);
        break;
    }
    const Standard& standard = high ? highDefinition : smallScreen;
    // In the narrow range luma runs from 16 to 235 and a difference from 16 to 240, of 255.
    const float luma = whole ? 1.0F : 255.0F / 219.0F;
    const float colour = whole ? 1.0F : 255.0F / 224.0F;
    const float black = whole ? 0.0F : 16.0F / 255.0F;
    const float middle = 128.0F / 255.0F;

    const float red = standard.redFromRed * colour;
    const float greenBlue = standard.greenFromBlue * colour;
    const float greenRed = standard.greenFromRed * colour;
    const float blue = standard.blueFromBlue * colour;
    return {
        {luma, 0.0F, red, -luma * black - red * middle},
        {luma, -greenBlue, -greenRed, -luma * black + (greenBlue + greenRed) * middle},
        {luma, blue, 0.0F, -luma * black - blue * middle},
    };
}

StreamCore::StreamCore(GpuCore& gpu, const Host& host, Codec codec)
    : m_gpu(gpu), m_host(host), m_codec(codec), m_waitingForKeyFrame(codec == Codec::H264 || codec == Codec::Rgb),
      m_decodingSeen(gpu.decodingChanges()), m_recoveredSeen(gpu.timesRecovered())
{
    m_gpu.attach(static_cast<CardDecoderHolder*>(this));
}

StreamCore::~StreamCore()
{
    // No recovery asks it for its decoder from here on, and so none wakes it as it lets go of its
    // decoding; then no thread decodes for it, and what woke it before is undone.
    m_gpu.detach(static_cast<CardDecoderHolder*>(this));
    m_gpu.decodePool().forget(this);
}

void StreamCore::push(const Packet& packet)
{
    // (A screen's changes are a key frame where they are the whole screen, as the packet says by its head.)
    const bool keyFrame = still() || (changes() ? base::screen::headOf(packet.data).value_or(base::screen::Head{}).whole
                                                : packet.keyFrame);
    const std::size_t limit = still() ? stillsWaiting : changes() ? changesWaiting : h264PacketsWaiting;
    const std::size_t longest = changes() ? base::screen::longest : maxPacketBytes;
    std::uint64_t dropped = 0;
    bool refused = false;
    bool taken = false;
    bool wish = false;
    bool same = false;

    if (packet.data.empty() || packet.data.size() > longest) {
        refused = true;
    } else {
        const std::lock_guard guard(m_inboxLock);
        bool take = true;
        if (still() && m_inbox.empty() && !m_stillInHand && !m_lastStillUndrawn &&
            m_lastStill.size == packet.data.size() &&
            std::memcmp(m_lastStill.bytes.data(), packet.data.data(), packet.data.size()) == 0) {
            // Byte for byte the still last decoded, while nothing waits (no newer one, no redraw after a
            // lost device) and none is being decoded: what is drawn is this picture already. Let go
            // (design 4, F7: a card whose machine's screen does not change is asked again and again).
            take = false;
            same = true;
        } else if (m_waitingForKeyFrame && !keyFrame) {
            // Without the key frame before it, this picture cannot be decoded.
            take = false;
            dropped = 1;
            wish = true;
        } else if (m_inbox.size() >= limit) {
            // (What only asks for the last picture again was never handed in, and is not counted.)
            dropped = static_cast<std::uint64_t>(
                std::ranges::count_if(m_inbox, [](const Waiting& old) { return !old.again; }));
            for (Waiting& old : m_inbox) {
                keepSpare(std::move(old.bytes));
            }
            m_inbox.clear();
            if (!keyFrame) {
                m_waitingForKeyFrame = true;
                take = false;
                ++dropped;
                wish = true;
            }
        }
        if (take) {
            m_waitingForKeyFrame = false;
            Waiting waiting;
            if (!m_spare.empty()) {
                waiting.bytes = std::move(m_spare.back());
                m_spare.pop_back();
            }
            waiting.size = packet.data.size();
            waiting.bytes.resize(waiting.size + AV_INPUT_BUFFER_PADDING_SIZE);
            std::memcpy(waiting.bytes.data(), packet.data.data(), waiting.size);
            std::memset(waiting.bytes.data() + waiting.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            waiting.arrivedUs = packet.arrivedUs;
            waiting.keyFrame = keyFrame;
            m_inbox.push_back(std::move(waiting));
            taken = true;
        }
    }

    {
        const std::lock_guard guard(m_statsLock);
        ++m_stats.pushed;
        m_stats.droppedBehind += dropped;
        m_stats.refused += refused ? 1 : 0;
        m_stats.stillsSame += same ? 1 : 0;
        if (packet.arrivedUs != 0) {
            if (m_lastArrivedUs != 0) {
                const double step = static_cast<double>(packet.arrivedUs - m_lastArrivedUs) / 1000.0;
                ++m_stats.arrivalGapCounts[placeAmong(gapStepsMs, step)];
            }
            m_lastArrivedUs = packet.arrivedUs;
        }
    }
    if (wish) {
        m_keyFrameWish.store(true, std::memory_order_release);
    }
    if (taken) {
        m_gpu.decodePool().wake(this);
    }
}

void StreamCore::shownAt(SizePx /*onScreen*/, Importance importance)
{
    // The size is not used yet: who gets the card is decided by importance alone until there are
    // measurements to decide it by (DESIGN.md section 3.5).
    m_importance.store(static_cast<int>(importance), std::memory_order_relaxed);
}

bool StreamCore::takeKeyFrameWish()
{
    return m_keyFrameWish.exchange(false, std::memory_order_acq_rel);
}

Stats StreamCore::stats() const
{
    const std::lock_guard guard(m_statsLock);
    return m_stats;
}

bool StreamCore::wantsTheCard() const
{
    if (m_codec != Codec::H264 || m_cardGaveUp || m_gpu.lost()) {
        return false;
    }
    switch (m_gpu.decodingChoice()) {
    case DecodingChoice::GraphicsCard:
        return true;
    case DecodingChoice::Software:
        return false;
    case DecodingChoice::Automatic:
        break;
    }
    return m_importance.load(std::memory_order_relaxed) >= static_cast<int>(Importance::ShownLarge);
}

void StreamCore::chooseDecoder()
{
    const bool card = wantsTheCard();
    if (m_decoder != nullptr && (m_decoder->kind() == Decoding::OnTheCard) == card) {
        return;
    }
    m_decoder.reset();
    if (card) {
        if (base::Result<std::unique_ptr<Decoder>> made = makeCardDecoder(m_gpu)) {
            m_decoder = std::move(made).value();
            return;
        }
        // The card is full or cannot: in software for now, and asked again at the next key frame.
    }
    if (base::Result<std::unique_ptr<Decoder>> made = makeSoftwareDecoder(m_codec)) {
        m_decoder = std::move(made).value();
    }
}

void StreamCore::run()
{
    // A copy of the screen to the card failed, and its time to be tried again has come (takeChanges): a draw is asked
    // of the host for it. Where a packet's run comes first, it asks a moment sooner, as the packet asks itself. (Before
    // the decoding is tried for: whoever holds that does not ask. Taken away, it is drawn no more.)
    if (m_drawForACopy.exchange(false, std::memory_order_acq_rel) && !m_retired.load(std::memory_order_acquire)) {
        if (urgent() && m_host.somethingUrgent) {
            m_host.somethingUrgent();
        } else if (m_host.somethingNew) {
            m_host.somethingNew();
        }
    }
    std::unique_lock decoding(m_decodingLock, std::try_to_lock);
    if (!decoding.owns_lock()) {
        return; // whoever holds it looks again as it lets go (letGoOfDecoding)
    }
    // Taken away: nothing more is decoded (2d's reading).
    if (m_retired.load(std::memory_order_acquire)) {
        letGoOfDecoding(decoding);
        return;
    }

    Waiting packet;
    bool none = false;
    {
        const std::lock_guard guard(m_inboxLock);
        none = m_inbox.empty();
        if (!none) {
            packet = std::move(m_inbox.front());
            m_inbox.pop_front();
        }
        m_stillInHand = !none && still() && !packet.again;
    }
    if (none) {
        letGoOfDecoding(decoding);
        return;
    }
    if (changes()) {
        applyChanges(packet);
        {
            const std::lock_guard guard(m_inboxLock);
            keepSpare(std::move(packet.bytes));
        }
        letGoOfDecoding(decoding);
        return;
    }
    // The last picture again, after a lost device took the one drawn: as it is now, the newest
    // decoded, unless a newer one waits to be drawn already (and of a stream that never decoded
    // one, nothing).
    if (packet.again && (m_lastStill.size == 0 || hasNewer())) {
        letGoOfDecoding(decoding);
        return;
    }
    const std::span<std::byte> bytes =
        packet.again ? std::span<std::byte>(m_lastStill.bytes) : std::span<std::byte>(packet.bytes);
    const std::size_t size = packet.again ? m_lastStill.size : packet.size;

    const std::int64_t began = base::steadyNowUs();
    // Decoding was chosen anew, or the device was lost and is back: how the stream is to be decoded
    // is looked at again (once the device is there to say what it can: while it is lost, this is
    // looked at when it is back). A key frame decoded while the device was gone was decoded in
    // software; one made anew may decode on its card again. Chosen to be on the card, or on a
    // device made anew, a card that gave up on the stream is tried again; another choice leaves
    // it as it is.
    const std::uint64_t seenBefore = m_decodingSeen;
    const std::uint64_t recoveredBefore = m_recoveredSeen;
    bool chosenAnew = false;
    if (const std::uint64_t changes = m_gpu.decodingChanges(), recovered = m_gpu.timesRecovered();
        (changes != m_decodingSeen || recovered != m_recoveredSeen) && !m_gpu.lost()) {
        if (m_gpu.decodingChoice() == DecodingChoice::GraphicsCard || recovered != m_recoveredSeen) {
            m_cardGaveUp = false;
        }
        m_decodingSeen = changes;
        m_recoveredSeen = recovered;
        chosenAnew = true;
    }
    // A stream changes the way it is decoded at a key frame, and nowhere else.
    if (packet.keyFrame) {
        chooseDecoder();
    }
    // Where the new choice changes how this stream is decoded, and the change can be made, it is
    // made at the next key frame, which is wished for. Where it changes nothing, where the card
    // could not take the stream, or where this packet was a key frame that made the change
    // already, nothing is asked of anybody: a server may hold back every picture until the key
    // frame it was asked for.
    if (chosenAnew && m_decoder != nullptr) {
        const bool onTheCard = m_decoder->kind() == Decoding::OnTheCard;
        const bool toTheCard = wantsTheCard() && (onTheCard || m_gpu.mayMakeCardDecoder());
        if (m_gpu.lost()) {
            // Lost since it was looked at: what the device can do is not known, and the choice is
            // looked at again when it is back.
            m_decodingSeen = seenBefore;
            m_recoveredSeen = recoveredBefore;
        } else if (toTheCard != onTheCard) {
            m_keyFrameWish.store(true, std::memory_order_release);
        }
    }

    bool failed = m_decoder == nullptr;
    Picture picture;
    double took = 0;
    Decoding how = Decoding::NotYet;
    if (!failed) {
        base::Result<Picture> decoded = m_decoder->decode(bytes, size, packet.arrivedUs);
        if (!decoded && decoded.error().code != refusedByTheGate && m_decoder->kind() == Decoding::OnTheCard &&
            packet.keyFrame && !m_gpu.lost()) {
            // The card cannot decode this stream (larger than it takes, a kind it does not have):
            // in software then, from this same key frame on. (Not where the packet was refused before any
            // decoder had it, by the SPS gate: the packet is at fault, not the card.)
            m_cardGaveUp = true;
            chooseDecoder();
            if (m_decoder != nullptr) {
                decoded = m_decoder->decode(bytes, size, packet.arrivedUs);
            }
        }
        if (!decoded) {
            failed = true;
        } else if (decoded.value()) {
            took = static_cast<double>(base::steadyNowUs() - began) / 1000.0;
            picture = std::move(decoded).value();
            picture.beganUs = began;
            how = m_decoder->kind();
        }
    }

    {
        const std::lock_guard guard(m_inboxLock);
        // A still that made a picture is kept as it came, before the picture is told of: whoever sees it
        // decoded finds it the last still decoded (push lets the same again go). One whose decoding
        // failed never is, and is tried again when it comes again. The one kept before goes back to be
        // used again.
        if (still() && picture && !packet.again) {
            std::swap(m_lastStill.bytes, packet.bytes);
            m_lastStill.size = packet.size;
            m_lastStill.keyFrame = true;
            m_lastStillUndrawn = false;
        }
        m_stillInHand = false;
        keepSpare(std::move(packet.bytes));
    }
    if (picture) {
        publish(std::move(picture), took, how);
    }
    if (failed) {
        // A lost device is told to the host as that, once, and not as each stream's failure.
        startAgain(!m_gpu.lost());
    }
    letGoOfDecoding(decoding);
}

void StreamCore::retire()
{
    m_retired.store(true, std::memory_order_release);
    const std::lock_guard guard(m_inboxLock);
    m_inbox.clear();
}

void StreamCore::letGoOfDecoding(std::unique_lock<std::mutex>& decoding)
{
    decoding.unlock();
    bool more = false;
    {
        const std::lock_guard guard(m_inboxLock);
        more = !m_inbox.empty();
    }
    if (more) {
        m_gpu.decodePool().wake(this);
    }
}

void StreamCore::startAgain(bool tellTheHost)
{
    m_decoder.reset();
    std::uint64_t dropped = 0;
    {
        const std::lock_guard guard(m_inboxLock);
        if (m_codec == Codec::H264) {
            // What waits was made from pictures that are lost now.
            dropped = m_inbox.size();
            for (Waiting& old : m_inbox) {
                keepSpare(std::move(old.bytes));
            }
            m_inbox.clear();
            m_waitingForKeyFrame = true;
        }
    }
    {
        const std::lock_guard guard(m_statsLock);
        ++m_stats.refused;
        m_stats.droppedBehind += dropped;
    }
    m_keyFrameWish.store(true, std::memory_order_release);
    if (tellTheHost && m_host.fault) {
        m_host.fault(Fault::StreamFailed);
    }
}

void StreamCore::publish(Picture picture, double decodeMs, Decoding how)
{
    const SizePx size{picture.frame->width, picture.frame->height};
    picture.decodedUs = base::steadyNowUs();
    bool replaced = false;
    {
        const std::lock_guard guard(m_slotLock);
        replaced = static_cast<bool>(m_newest);
        m_newest = std::move(picture);
    }
    {
        const std::lock_guard guard(m_statsLock);
        ++m_stats.decoded;
        m_stats.replaced += replaced ? 1 : 0;
        m_stats.decodeMs = towards(m_stats.decodeMs, decodeMs);
        m_stats.decoding = how;
        m_stats.picture = size;
    }
    // A machine somebody looks at closely, or controls, is drawn as soon as can be; a tile of a wall
    // at the host's own pace, with whatever else came meanwhile.
    if (urgent() && m_host.somethingUrgent) {
        m_host.somethingUrgent();
    } else if (m_host.somethingNew) {
        m_host.somethingNew();
    }
}

void StreamCore::dropCardDecoder()
{
    std::unique_lock decoding(m_decodingLock);
    const bool had = m_decoder != nullptr && m_decoder->kind() == Decoding::OnTheCard;
    if (had) {
        m_decoder.reset();
    }
    {
        const std::lock_guard guard(m_slotLock);
        if (m_newest.onTheCard()) {
            m_newest = Picture{};
        }
    }
    if (had) {
        {
            const std::lock_guard guard(m_inboxLock);
            for (Waiting& old : m_inbox) {
                keepSpare(std::move(old.bytes));
            }
            m_inbox.clear();
            m_waitingForKeyFrame = true;
        }
        m_keyFrameWish.store(true, std::memory_order_release);
    }
    letGoOfDecoding(decoding);
}

bool StreamCore::hasNewer() const
{
    const std::lock_guard guard(m_slotLock);
    return static_cast<bool>(m_newest) || m_changesWaiting;
}

void StreamCore::waitForAWholeScreen()
{
    std::uint64_t dropped = 0;
    {
        const std::lock_guard guard(m_inboxLock);
        dropped = m_inbox.size();
        for (Waiting& old : m_inbox) {
            keepSpare(std::move(old.bytes));
        }
        m_inbox.clear();
        m_waitingForKeyFrame = true;
    }
    {
        const std::lock_guard guard(m_statsLock);
        m_stats.droppedBehind += dropped;
    }
    m_keyFrameWish.store(true, std::memory_order_release);
}

void StreamCore::keepSpare(std::vector<std::byte>&& bytes)
{
    // As many as wait at once and one more for the stream's own packets, which are small (a key frame of H.264, a
    // JPEG); two for a screen's changes, each as long as the screen whole may be (megabytes): one being filled
    // while one is applied.
    const std::size_t most = changes() ? 2 : still() ? stillsWaiting + 1 : h264PacketsWaiting + 1;
    if (!bytes.empty() && m_spare.size() < most) {
        m_spare.push_back(std::move(bytes));
    }
}

void StreamCore::applyChanges(const Waiting& packet)
{
    const std::int64_t began = base::steadyNowUs();
    const std::optional<base::screen::Changes> read =
        base::screen::read(std::span<const std::byte>(packet.bytes).first(packet.size));
    bool applied = false;
    bool notOfThisScreen = false;
    SizePx size;
    if (read) {
        const std::lock_guard guard(m_screenLock);
        const bool sameScreen =
            m_screen != nullptr && m_screen->width == read->width && m_screen->height == read->height;
        if (!sameScreen && read->whole()) {
            // A whole screen of another size, or the first: a screen of its own, made anew (and what changed is all).
            FramePtr made(av_frame_alloc());
            if (made != nullptr) {
                made->format = AV_PIX_FMT_GBRP;
                made->width = read->width;
                made->height = read->height;
                if (av_frame_get_buffer(made.get(), 0) < 0) {
                    made.reset();
                }
            }
            m_screen = std::move(made);
            m_dirty.clear();
            m_dirtyAll = true;
        }
        if (m_screen != nullptr && m_screen->width == read->width && m_screen->height == read->height) {
            AVFrame& screen = *m_screen;
            const std::byte* from = read->pixels.data();
            for (const base::screen::Rect& rect : read->rects) {
                for (int y = rect.y; y < rect.y + rect.height; ++y) {
                    std::uint8_t* const green = screen.data[0] + static_cast<std::ptrdiff_t>(y) * screen.linesize[0];
                    std::uint8_t* const blue = screen.data[1] + static_cast<std::ptrdiff_t>(y) * screen.linesize[1];
                    std::uint8_t* const red = screen.data[2] + static_cast<std::ptrdiff_t>(y) * screen.linesize[2];
                    for (int x = rect.x; x < rect.x + rect.width; ++x) {
                        red[x] = static_cast<std::uint8_t>(from[0]);
                        green[x] = static_cast<std::uint8_t>(from[1]);
                        blue[x] = static_cast<std::uint8_t>(from[2]);
                        from += 3;
                    }
                }
                if (!m_dirtyAll) {
                    m_dirty.push_back({rect.x, rect.y, rect.width, rect.height});
                }
            }
            if (m_dirty.size() > mostRectsToCopy) {
                m_dirty.clear();
                m_dirtyAll = true;
            }
            size = {screen.width, screen.height};
            applied = true;
            if (const int ms = m_gpu.applyingMs(); ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms)); // (a busy screen, for tests: slowApplying)
            }
        } else {
            notOfThisScreen = true;
        }
    }
    if (!applied) {
        // Not a packet of changes, or changes of a screen this stream does not hold (another size, or none yet): what
        // they changed is lost, and nothing after them is right before a whole screen comes.
        {
            const std::lock_guard guard(m_statsLock);
            if (notOfThisScreen) {
                ++m_stats.droppedBehind;
            } else {
                ++m_stats.refused;
            }
        }
        waitForAWholeScreen();
        return;
    }
    const std::int64_t decoded = base::steadyNowUs();
    bool replaced = false;
    {
        const std::lock_guard guard(m_slotLock);
        replaced = m_changesWaiting;
        m_changesWaiting = true;
        m_changesArrivedUs = packet.arrivedUs;
        m_changesBeganUs = began;
        m_changesDecodedUs = decoded;
    }
    {
        const std::lock_guard guard(m_statsLock);
        ++m_stats.decoded;
        m_stats.replaced += replaced ? 1 : 0;
        m_stats.decodeMs = towards(m_stats.decodeMs, static_cast<double>(decoded - began) / 1000.0);
        m_stats.decoding = Decoding::InSoftware;
        m_stats.picture = size;
    }
    if (urgent() && m_host.somethingUrgent) {
        m_host.somethingUrgent();
    } else if (m_host.somethingNew) {
        m_host.somethingNew();
    }
}

bool StreamCore::takeChanges(std::int64_t untilUs)
{
    std::int64_t arrived = 0;
    std::int64_t began = 0;
    std::int64_t decoded = 0;
    {
        const std::lock_guard guard(m_slotLock);
        if (!m_changesWaiting) {
            return false;
        }
        if (m_drawn.planes != 0 && base::steadyNowUs() > untilUs) {
            return true; // (copied at the next draw: what is drawn meanwhile is the screen as it was)
        }
        m_changesWaiting = false;
        arrived = m_changesArrivedUs;
        began = m_changesBeganUs;
        decoded = m_changesDecodedUs;
    }
    // What changed is waited for again, as it was (unless newer changes were applied meanwhile, which said so
    // themselves): the copy is left for the next draw.
    const auto waitAgain = [this, arrived, began, decoded] {
        const std::lock_guard guard(m_slotLock);
        if (!m_changesWaiting) {
            m_changesWaiting = true;
            m_changesArrivedUs = arrived;
            m_changesBeganUs = began;
            m_changesDecodedUs = decoded;
        }
    };
    // Only tried: a packet that is being applied holds the screen for as long as that takes (milliseconds for a busy
    // 1080p screen), and this is the drawing, with the device's lock held, which every other stream's drawing and the
    // card's decoding wait for. The screen as it was stays drawn, and the packet's end asks for a draw (applyChanges).
    std::unique_lock screenHeld(m_screenLock, std::try_to_lock);
    if (!screenHeld.owns_lock()) {
        waitAgain();
        return false;
    }
    const std::int64_t taken = base::steadyNowUs();
    bool copied = false;
    std::uint64_t copiedBytes = 0;
    {
        if (m_screen == nullptr) {
            return false;
        }
        const AVFrame& screen = *m_screen;
        const bool onTheCard = m_drawn.planes == 3 && m_planesFormat == AV_PIX_FMT_GBRP &&
                               m_planes[0].width == screen.width && m_planes[0].height == screen.height;
        m_couldNotMake = false;
        if (!onTheCard || m_dirtyAll) {
            copied = copyToTheCard(screen); // (the whole screen, and the planes made for it where they are not)
            copiedBytes = copied ? planeBytes() : 0;
        } else {
            ID3D11DeviceContext* context = m_gpu.context();
            for (const RectPx& rect : m_dirty) {
                const D3D11_BOX box{static_cast<UINT>(rect.x),
                                    static_cast<UINT>(rect.y),
                                    0,
                                    static_cast<UINT>(rect.x + rect.width),
                                    static_cast<UINT>(rect.y + rect.height),
                                    1};
                for (int plane = 0; plane < 3; ++plane) {
                    const std::uint8_t* const first =
                        screen.data[plane] + static_cast<std::ptrdiff_t>(rect.y) * screen.linesize[plane] + rect.x;
                    context->UpdateSubresource(m_planes[plane].texture.Get(), 0, &box, first,
                                               static_cast<UINT>(screen.linesize[plane]), 0);
                }
                copiedBytes +=
                    std::uint64_t{3} * static_cast<std::uint64_t>(rect.width) * static_cast<std::uint64_t>(rect.height);
            }
            copied = true;
        }
        // (What could not be copied is copied whole: the planes went with the copy that failed.)
        m_dirty.clear();
        m_dirtyAll = false;
    }
    screenHeld.unlock();
    if (!copied) {
        // (A texture could not be made: the card's memory was full, say.) Tried again at a draw that is asked of the
        // host for it, also where the server's screen changes no more (one at its BIOS, or a login prompt, sends
        // nothing by itself) and nothing else draws: asked by run() when the pool wakes this at its time, which grows
        // with each failure in a row. (It was left for a draw that came for another reason, and none may come. Asked
        // at once, by the scene, a card that stays full would be tried at every frame, the window drawn as fast as it
        // can be.)
        waitAgain();
        m_copyAgainInUs = m_copyAgainInUs == 0 ? firstCopyAgainUs : std::min(2 * m_copyAgainInUs, longestCopyAgainUs);
        m_drawForACopy.store(true, std::memory_order_release);
        m_gpu.decodePool().wakeAt(this, base::steadyNowUs() + m_copyAgainInUs);
        const std::lock_guard guard(m_statsLock);
        ++m_stats.refused;
        return false;
    }
    // (Copied: a try that was still to come asks for nothing, and the next failure waits the shortest time again.)
    m_copyAgainInUs = 0;
    m_drawForACopy.store(false, std::memory_order_release);
    m_drawnArrivedUs = arrived;
    m_drawnBeganUs = began;
    m_drawnDecodedUs = decoded;
    m_drawnTakenUs = taken;
    m_drawnIsNew = true;
    const std::lock_guard guard(m_statsLock);
    m_stats.uploadedBytes += copiedBytes;
    m_stats.uploadMs = towards(m_stats.uploadMs, static_cast<double>(base::steadyNowUs() - taken) / 1000.0);
    return false;
}

std::uint64_t StreamCore::planeBytes() const noexcept
{
    std::uint64_t bytes = 0;
    for (const Plane& plane : m_planes) {
        bytes += static_cast<std::uint64_t>(plane.width) * static_cast<std::uint64_t>(plane.height);
    }
    return bytes;
}

bool StreamCore::takeNewer(std::int64_t untilUs)
{
    if (changes()) {
        return takeChanges(untilUs);
    }
    Picture next;
    {
        const std::lock_guard guard(m_slotLock);
        if (!m_newest) {
            return false;
        }
        if (!m_newest.onTheCard() && m_drawn.planes != 0 && base::steadyNowUs() > untilUs) {
            return true;
        }
        next = std::move(m_newest);
    }

    const std::int64_t began = base::steadyNowUs();
    const bool card = next.onTheCard();
    m_couldNotMake = false;
    if (!(card ? drawFromTheCard(next) : copyToTheCard(*next.frame))) {
        if (still() && !m_gpu.lost() && m_couldNotMake) {
            // Decoded and never drawn because a texture or a view could not be made (the card's memory
            // was full, say): the same still again is no picture on the screen already, and is decoded
            // again (2c, after its reading). One the engine cannot draw at all (a kind of picture it does
            // not draw) is not tried again and again (2c, the second reading). After a lost device the
            // last still is decoded again anyway.
            const std::lock_guard guard(m_inboxLock);
            m_lastStillUndrawn = true;
        }
        const std::lock_guard guard(m_statsLock);
        ++m_stats.refused;
        return false;
    }
    m_drawnArrivedUs = next.arrivedUs;
    m_drawnBeganUs = next.beganUs;
    m_drawnDecodedUs = next.decodedUs;
    m_drawnTakenUs = began;
    m_drawnIsNew = true;
    if (!card) {
        const std::uint64_t copied = planeBytes();
        const std::lock_guard guard(m_statsLock);
        m_stats.uploadedBytes += copied;
        m_stats.uploadMs = towards(m_stats.uploadMs, static_cast<double>(base::steadyNowUs() - began) / 1000.0);
    }
    return false;
}

bool StreamCore::makePlanes(const AVFrame& frame, int count, int shiftX, int shiftY)
{
    ID3D11Device* device = m_gpu.device();
    if (device == nullptr) {
        return false;
    }
    // What is drawn may be views of these planes, which go now: nothing is drawn from them after this,
    // also where the new ones cannot be made (2c, after its reading).
    m_drawn = Drawable{};
    m_held.reset();
    for (Plane& plane : m_planes) {
        plane = Plane{};
    }
    m_planesFormat = AV_PIX_FMT_NONE;
    for (int index = 0; index < count; ++index) {
        Plane& plane = m_planes[index];
        plane.width = index == 0 ? frame.width : shifted(frame.width, shiftX);
        plane.height = index == 0 ? frame.height : shifted(frame.height, shiftY);
        D3D11_TEXTURE2D_DESC wanted{};
        wanted.Width = static_cast<UINT>(plane.width);
        wanted.Height = static_cast<UINT>(plane.height);
        wanted.MipLevels = 1;
        wanted.ArraySize = 1;
        wanted.Format = DXGI_FORMAT_R8_UNORM;
        wanted.SampleDesc.Count = 1;
        // An ordinary texture, written to whole for every picture. A "dynamic" one that is mapped
        // looks made for this and is not: with sixteen 1080p30 streams it kept the card's drawing
        // more than three times as busy, and pictures were lost (measured).
        wanted.Usage = D3D11_USAGE_DEFAULT;
        wanted.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (m_gpu.textureFails() || FAILED(device->CreateTexture2D(&wanted, nullptr, &plane.texture)) ||
            FAILED(device->CreateShaderResourceView(plane.texture.Get(), nullptr, &plane.view))) {
            m_couldNotMake = true;
            return false;
        }
    }
    if (count == 1 && m_grey == nullptr) {
        // A picture without colour is drawn with both differences in the middle: one grey pixel.
        const std::uint8_t middle = 128;
        D3D11_TEXTURE2D_DESC wanted{};
        wanted.Width = 1;
        wanted.Height = 1;
        wanted.MipLevels = 1;
        wanted.ArraySize = 1;
        wanted.Format = DXGI_FORMAT_R8_UNORM;
        wanted.SampleDesc.Count = 1;
        wanted.Usage = D3D11_USAGE_IMMUTABLE;
        wanted.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA content{&middle, 1, 1};
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&wanted, &content, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &m_grey))) {
            m_couldNotMake = true;
            return false;
        }
    }
    m_planesFormat = static_cast<AVPixelFormat>(frame.format);
    return true;
}

bool StreamCore::copyToTheCard(const AVFrame& frame)
{
    const Layout layout = layoutOf(frame.format);
    if (layout.planes == 0) {
        return false; // a kind of picture the engine does not draw
    }
    if (m_planesFormat != frame.format || m_planes[0].width != frame.width || m_planes[0].height != frame.height) {
        if (!makePlanes(frame, layout.planes, layout.shiftX, layout.shiftY)) {
            return false;
        }
    }
    ID3D11DeviceContext* context = m_gpu.context();
    for (int index = 0; index < layout.planes; ++index) {
        const Plane& plane = m_planes[index];
        const int stride = frame.linesize[index];
        if (frame.data[index] == nullptr || stride < plane.width) {
            return false;
        }
        // Direct3D takes the rows as they lie in memory, each `stride` bytes after the one before.
        context->UpdateSubresource(plane.texture.Get(), 0, nullptr, frame.data[index], static_cast<UINT>(stride), 0);
    }

    m_held.reset();
    // A decoder on the card's texture is of no use to a picture drawn from memory: let go of (2d's reading).
    m_cardViews.clear();
    m_drawn = Drawable{};
    m_drawn.planes = 3;
    m_drawn.plane[0] = m_planes[0].view;
    m_drawn.plane[1] = layout.planes == 3 ? m_planes[1].view : m_grey;
    m_drawn.plane[2] = layout.planes == 3 ? m_planes[2].view : m_grey;
    m_drawn.picture = {frame.width, frame.height};
    m_drawn.colours = colourRowsFor(frame);
    return true;
}

bool StreamCore::drawFromTheCard(Picture& picture)
{
    ID3D11Device* device = m_gpu.device();
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(picture.frame->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(picture.frame->data[1]));
    if (device == nullptr || texture == nullptr) {
        return false;
    }
    // A decoder that was made anew has a texture of its own; what is kept of the old one's only
    // holds its memory. What is drawn from those goes first, also where the new views cannot be made
    // (2c, the second reading).
    if (!m_cardViews.empty() && m_cardViews.front().texture != texture) {
        m_drawn = Drawable{};
        m_held.reset();
        m_cardViews.clear();
    }
    auto views = std::ranges::find_if(m_cardViews, [&](const CardViews& known) { return known.slice == slice; });
    if (views == m_cardViews.end()) {
        CardViews made;
        made.texture = texture;
        made.slice = slice;
        // The decoder's texture holds every picture as one slice: luma in full size, then the two
        // differences side by side at half the size. A view names the slice and which of the two.
        D3D11_SHADER_RESOURCE_VIEW_DESC wanted{};
        wanted.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        wanted.Texture2DArray.MipLevels = 1;
        wanted.Texture2DArray.FirstArraySlice = slice;
        wanted.Texture2DArray.ArraySize = 1;
        wanted.Format = DXGI_FORMAT_R8_UNORM;
        if (m_gpu.textureFails() || FAILED(device->CreateShaderResourceView(texture, &wanted, &made.luma))) {
            m_couldNotMake = true;
            return false;
        }
        wanted.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device->CreateShaderResourceView(texture, &wanted, &made.colour))) {
            m_couldNotMake = true;
            return false;
        }
        m_cardViews.push_back(std::move(made));
        views = m_cardViews.end() - 1;
    }

    // The texture is as large as the decoder needs it, which is the picture rounded up to whole blocks.
    D3D11_TEXTURE2D_DESC is{};
    texture->GetDesc(&is);
    m_drawn = Drawable{};
    m_drawn.planes = 2;
    m_drawn.plane[0] = views->luma;
    m_drawn.plane[1] = views->colour;
    m_drawn.picture = {picture.frame->width, picture.frame->height};
    m_drawn.cropRight = static_cast<float>(picture.frame->width) / static_cast<float>(is.Width);
    m_drawn.cropBottom = static_cast<float>(picture.frame->height) / static_cast<float>(is.Height);
    m_drawn.colours = colourRowsFor(*picture.frame);
    m_held = std::move(picture.frame);
    return true;
}

Drawable StreamCore::drawable() const
{
    return m_drawn;
}

void StreamCore::noteShown(std::int64_t atUs)
{
    if (!m_drawnIsNew) {
        return;
    }
    m_drawnIsNew = false;
    const std::lock_guard guard(m_statsLock);
    ++m_stats.shown;
    if (m_lastShownUs != 0) {
        const double step = static_cast<double>(atUs - m_lastShownUs) / 1000.0;
        ++m_stats.shownGapCounts[placeAmong(gapStepsMs, step)];
        m_stats.shownGapMaxMs = std::max(m_stats.shownGapMaxMs, step);
    }
    m_lastShownUs = atUs;
    if (m_drawnArrivedUs == 0) {
        m_lastTookMs = -1;
        return; // the host did not say when the packet arrived
    }
    const double took = static_cast<double>(atUs - m_drawnArrivedUs) / 1000.0;
    m_stats.arrivedToShownMs = towards(m_stats.arrivedToShownMs, took);
    m_stats.arrivedToShownMaxMs = std::max(m_stats.arrivedToShownMaxMs, took);
    ++m_stats.arrivedToShownCounts[placeAmong(arrivedToShownStepsMs, took)];
    if (m_lastTookMs >= 0) {
        ++m_stats.unevenCounts[placeAmong(unevenStepsMs, std::abs(took - m_lastTookMs))];
    }
    m_lastTookMs = took;
    if (m_drawnBeganUs != 0 && m_drawnDecodedUs != 0 && m_drawnTakenUs != 0) {
        const double waited = static_cast<double>(m_drawnBeganUs - m_drawnArrivedUs) / 1000.0;
        const double ready = static_cast<double>(m_drawnTakenUs - m_drawnDecodedUs) / 1000.0;
        const double frame = static_cast<double>(atUs - m_drawnTakenUs) / 1000.0;
        m_stats.waitedMs = towards(m_stats.waitedMs, waited);
        m_stats.readyToDrawnMs = towards(m_stats.readyToDrawnMs, ready);
        m_stats.drawnToShownMs = towards(m_stats.drawnToShownMs, frame);
        ++m_stats.readyToDrawnCounts[placeAmong(arrivedToShownStepsMs, ready)];
        ++m_stats.drawnToShownCounts[placeAmong(arrivedToShownStepsMs, frame)];
    }
}

void StreamCore::noteNotShown()
{
    const std::lock_guard guard(m_statsLock);
    m_lastShownUs = 0;
    m_lastTookMs = -1;
}

void StreamCore::dropDrawObjects()
{
    // Nothing is drawn until the device is back: no step on the screen is counted across that.
    noteNotShown();
    // And the picture that was drawn goes with the device: an H.264 stream asks for a key frame to
    // be drawn again from, also where it was decoded in software (one on the card asks already, as
    // its decoder goes), and also where the KVM's screen stands still and sends nothing by itself.
    // A JPEG stream decodes its last picture again, unless a newer one comes: where none waits, it
    // is asked for, and the decoding decides when it comes to it, which picture is the newest then
    // (one decoded meanwhile is). Nothing is copied here, on the drawing thread.
    if (m_codec == Codec::H264) {
        m_keyFrameWish.store(true, std::memory_order_release);
    } else if (changes()) {
        // A screen's changes: the screen as it stands is in this program's memory, and is copied whole to the card
        // that comes back (at the next draw: nothing is copied here, and the screen is not waited for: a packet may be
        // being applied to it; the planes go below, so the copy is of the whole). Drawn again, not arrived: no time
        // from an arrival is counted for it, as for a still's.
        {
            const std::lock_guard guard(m_slotLock);
            m_changesWaiting = true;
            m_changesArrivedUs = 0;
            m_changesBeganUs = 0;
            m_changesDecodedUs = 0;
        }
        if (m_host.somethingNew) {
            m_host.somethingNew();
        }
    } else {
        bool again = false;
        {
            const std::lock_guard guard(m_inboxLock);
            if (m_inbox.empty()) {
                Waiting last;
                last.again = true;
                last.keyFrame = true;
                m_inbox.push_back(std::move(last));
                again = true;
            }
        }
        if (again) {
            m_gpu.decodePool().wake(this);
        }
    }
    m_drawn = Drawable{};
    m_held.reset();
    m_drawnIsNew = false;
    for (Plane& plane : m_planes) {
        plane = Plane{};
    }
    m_grey.Reset();
    m_planesFormat = AV_PIX_FMT_NONE;
    m_cardViews.clear();
}

} // namespace qkvm::engine
