#include "jpeg_pixels.h"

#include <algorithm>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace qkvm::core {

namespace {

struct FreeFrame {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
struct FreePacket {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
struct FreeContext {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};

base::Error garbled(const char* why)
{
    return base::Error{"garbled", why};
}

std::uint8_t clamped(int value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

struct JpegPixels::State {
    std::unique_ptr<AVCodecContext, FreeContext> context;
    std::unique_ptr<AVPacket, FreePacket> packet{av_packet_alloc()};
    std::unique_ptr<AVFrame, FreeFrame> frame{av_frame_alloc()};
    std::vector<std::uint8_t> padded;
    std::int64_t madeFor = -1; // the most pixels the decoder was made for
};

JpegPixels::JpegPixels() : m_state(std::make_unique<State>()) {}

JpegPixels::~JpegPixels() = default;

namespace {

// What a strict walk of a whole JPEG found: whether it is one this program will decode at all, its frame kind (the SOF
// marker) and the screen it says. FFmpeg cannot be stopped once it is decoding, so a Tight JPEG is held to exactly the
// shape a real server sends before FFmpeg sees a byte (finding 1 of the confirm review): one baseline (or extended
// sequential) frame, one scan for each colour at the most, their entropy-coded data walked properly, then EOI and
// nothing after. A second frame, a scan too many, or anything else is refused unread.
struct Scanned {
    bool ok = false;
    int sof = 0;
    int width = 0;
    int height = 0;
};

// Markers a JPEG may hold in all: a stream of far more is no picture a server has reason to send.
constexpr int mostMarkers = 64;
// The longest a single baseline decode may take: a 2048x8192 one is tens of milliseconds, so this is generous and
// catches only a decoder gone astray.
constexpr std::int64_t decodeBudgetMs = 2000;

// The next marker from `at`, skipping 0xFF fill bytes (T.81 B.1.1.2 allows any number of 0xFF before a marker): the
// offset of its 0xFF in `markerAt` and the marker byte returned, or 0 where none is left. `at` must be at a 0xFF.
std::uint8_t markerFrom(std::span<const std::uint8_t> jpeg, std::size_t at, std::size_t& markerAt)
{
    while (at + 1 < jpeg.size() && jpeg[at] == 0xFF && jpeg[at + 1] == 0xFF) {
        ++at; // a 0xFF fill byte before the marker
    }
    if (at + 1 >= jpeg.size() || jpeg[at] != 0xFF) {
        return 0;
    }
    markerAt = at;
    return jpeg[at + 1];
}

// The entropy-coded data of a scan, from `at`: 0xFF00 is a stuffed 0xFF, RST0 to RST7 are part of it, and 0xFF fill
// bytes may come before any of these or before the marker that ends the scan. Returns the offset of that marker's
// 0xFF; `found` is set false where the data runs off the end with no marker.
std::size_t pastEntropy(std::span<const std::uint8_t> jpeg, std::size_t at, bool& found)
{
    found = false;
    while (at < jpeg.size()) {
        if (jpeg[at] != 0xFF) {
            ++at;
            continue;
        }
        std::size_t fill = at;
        while (fill + 1 < jpeg.size() && jpeg[fill + 1] == 0xFF) {
            ++fill; // 0xFF fill bytes
        }
        if (fill + 1 >= jpeg.size()) {
            return jpeg.size(); // a trailing run of 0xFF with nothing after: no marker
        }
        const std::uint8_t next = jpeg[fill + 1];
        if (next == 0x00) {
            at = fill + 2; // a stuffed 0xFF: still the data
            continue;
        }
        if (next >= 0xD0 && next <= 0xD7) {
            at = fill + 2; // a restart marker: still the scan's data
            continue;
        }
        found = true;
        return fill; // the marker that ends the scan
    }
    return at;
}

// A quantisation table segment's body (DQT), held to its exact shape: a run of tables, each a byte (Pq, Tq) then
// 64*(1+Pq) bytes, that exactly fills the body. FFmpeg reads whole tables and then looks for markers from where it
// stopped, so a tail left over by a claimed length could hide a marker (round-3 finding 2).
bool wholeDqt(std::span<const std::uint8_t> body)
{
    std::size_t at = 0;
    while (at < body.size()) {
        const int pq = body[at] >> 4;
        const int tq = body[at] & 0x0F;
        if (pq > 1 || tq > 3) {
            return false;
        }
        at += std::size_t{1} + std::size_t{64} * static_cast<std::size_t>(1 + pq);
        if (at > body.size()) {
            return false;
        }
    }
    return at == body.size();
}

// A Huffman table segment's body (DHT), the same way: a run of tables, each a byte (Tc, Th), sixteen counts, then the
// sum of the counts in symbols.
bool wholeDht(std::span<const std::uint8_t> body)
{
    std::size_t at = 0;
    while (at < body.size()) {
        const int tc = body[at] >> 4;
        const int th = body[at] & 0x0F;
        if (tc > 1 || th > 3 || at + 17 > body.size()) {
            return false;
        }
        std::size_t sum = 0;
        for (std::size_t one = 0; one < 16; ++one) {
            sum += body[at + 1 + one];
        }
        at += 17 + sum;
        if (at > body.size()) {
            return false;
        }
    }
    return at == body.size();
}

// Each marker held to its exact length rule (confirm review's round-2 finding 3): a marker skipped only by the length
// it claims lets a second frame and its scans hide in a segment FFmpeg's own parser reads markers inside (it reads no
// length for SOI, the JPGn and reserved markers, and treats a couple as JPEG-LS; it reads whole tables of DQT and DHT
// and then looks on). So only an allow-list is walked, each table's shape read whole, and anything else refused.
Scanned scan(std::span<const std::uint8_t> jpeg)
{
    Scanned found;
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return found;
    }
    std::size_t at = 2;
    int markers = 0;
    int components = 0;
    int scans = 0;
    bool sawEnd = false;
    while (at < jpeg.size()) {
        std::size_t markerAt = 0;
        const std::uint8_t marker = markerFrom(jpeg, at, markerAt);
        if (marker == 0) {
            return {}; // not at a marker where one is due
        }
        if (++markers > mostMarkers) {
            return {};
        }
        if (marker == 0xD9) { // EOI: no length, the end
            sawEnd = true;
            at = markerAt + 2;
            break;
        }
        // Everything from here has a two-byte length. A standalone marker (TEM, RSTn, SOI) or any marker not on the
        // allow-list below is refused: FFmpeg would read markers inside a segment this program skipped by a false
        // length.
        if (markerAt + 4 > jpeg.size()) {
            return {};
        }
        const std::size_t length = (std::size_t{jpeg[markerAt + 2]} << 8) | jpeg[markerAt + 3];
        if (length < 2 || markerAt + 2 + length > jpeg.size()) {
            return {};
        }
        const std::size_t body = markerAt + 4; // the first byte after the length
        // Every byte read below is inside the segment, which the length check above has held inside the span: the
        // minimum length is required before an index past the length field is read (round-3 finding 1, which crashed
        // the hardened build by reading one past the span).
        if (marker == 0xC0 || marker == 0xC1) {
            // A frame header (SOF), baseline or extended sequential Huffman only, exactly one, of length 8 + 3*Nf (so
            // at least 8: precision, height, width, Nf, then Nf component specs).
            if (found.sof != 0 || length < 8) {
                return {};
            }
            components = jpeg[markerAt + 9];
            if (components < 1 || components > 4 ||
                length != 8 + std::size_t{3} * static_cast<std::size_t>(components)) {
                return {};
            }
            found.sof = marker;
            found.height = (jpeg[markerAt + 5] << 8) | jpeg[markerAt + 6];
            found.width = (jpeg[markerAt + 7] << 8) | jpeg[markerAt + 8];
            at = markerAt + 2 + length;
        } else if (marker == 0xDA) {
            // A scan (SOS), of length 6 + 2*Ns (so at least 3 before Ns is read), then its entropy-coded data.
            if (found.sof == 0 || ++scans > components || length < 3) {
                return {};
            }
            const int many = jpeg[body];
            if (many < 1 || many > components || length != 6 + std::size_t{2} * static_cast<std::size_t>(many)) {
                return {};
            }
            bool nextMarker = false;
            at = pastEntropy(jpeg, markerAt + 2 + length, nextMarker);
            if (!nextMarker) {
                return {}; // the scan's data ran off the end with no EOI
            }
        } else if (marker == 0xDD) {
            if (length != 4) { // a restart interval (DRI) is always four
                return {};
            }
            at = markerAt + 2 + length;
        } else if (marker == 0xDB) {
            if (!wholeDqt(jpeg.subspan(body, length - 2))) { // a quantisation table read whole, not by claimed length
                return {};
            }
            at = markerAt + 2 + length;
        } else if (marker == 0xC4) {
            if (!wholeDht(jpeg.subspan(body, length - 2))) { // a Huffman table read whole
                return {};
            }
            at = markerAt + 2 + length;
        } else if (marker == 0xFE || (marker >= 0xE0 && marker <= 0xEF)) {
            // A comment (COM) or application segment (APPn): free-form, which FFmpeg also skips by its length.
            at = markerAt + 2 + length;
        } else {
            return {}; // anything else (another frame kind, JPGn, reserved, DNL, ...): refused unread
        }
    }
    if (!sawEnd || found.sof == 0 || scans == 0) {
        return {};
    }
    // After EOI only 0xFF fill or 0x00 padding may remain.
    for (; at < jpeg.size(); ++at) {
        if (jpeg[at] != 0xFF && jpeg[at] != 0x00) {
            return {};
        }
    }
    found.ok = true;
    return found;
}

} // namespace

std::optional<JpegPixels::Size> JpegPixels::sizeOf(std::span<const std::uint8_t> jpeg)
{
    const Scanned found = scan(jpeg);
    if (!found.ok) {
        return std::nullopt;
    }
    return Size{found.width, found.height};
}

base::Result<std::vector<std::uint8_t>> JpegPixels::decode(std::span<const std::uint8_t> jpeg, int width, int height)
{
    // `scan` already held it to the shape a real Tight JPEG has (finding 1): one baseline or extended sequential
    // frame, its scans and entropy data, EOI and nothing after. A progressive, lossless or arithmetic frame, a second
    // frame, or a scan too many never reaches FFmpeg, which cannot be stopped once it decodes.
    const Scanned found = scan(jpeg);
    if (!found.ok || found.width != width || found.height != height || width <= 0 || height <= 0) {
        return garbled("a JPEG that is not a baseline frame of its rectangle's size");
    }
    State& state = *m_state;
    if (state.packet == nullptr || state.frame == nullptr) {
        return base::Error{"no_memory", "There was no memory to decode a JPEG with"};
    }
    // A decoder that may make no larger a picture than this rectangle's.
    const std::int64_t pixels = std::int64_t{width} * height;
    if (state.context == nullptr || state.madeFor != pixels) {
        state.context.reset();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (codec == nullptr) {
            return garbled("this program has no JPEG decoder");
        }
        std::unique_ptr<AVCodecContext, FreeContext> context(avcodec_alloc_context3(codec));
        if (context == nullptr) {
            return base::Error{"no_memory", "There was no memory to decode a JPEG with"};
        }
        context->thread_count = 1;
        // (The decoder checks the picture as it is coded, its sides made whole blocks of up to 32: so much, and no
        // more.)
        const std::int64_t codedWidth = (std::int64_t{width} + 31) / 32 * 32;
        const std::int64_t codedHeight = (std::int64_t{height} + 31) / 32 * 32;
        context->max_pixels = codedWidth * codedHeight;
        if (avcodec_open2(context.get(), codec, nullptr) < 0) {
            return garbled("the JPEG decoder could not be opened");
        }
        state.context = std::move(context);
        state.madeFor = pixels;
    }
    // FFmpeg reads a little past what it is given: the JPEG, and zeros after it.
    state.padded.assign(jpeg.begin(), jpeg.end());
    state.padded.resize(jpeg.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    av_packet_unref(state.packet.get());
    state.packet->data = state.padded.data();
    state.packet->size = static_cast<int>(jpeg.size());
    av_frame_unref(state.frame.get());
    // Timed, as a last guard: a baseline frame of a bounded size decodes in well under this; one that does not is no
    // picture a server has reason to send, and its session is ended (the web kills such a worker: video.WORK_WITHIN).
    const auto began = std::chrono::steady_clock::now();
    const bool decoded = avcodec_send_packet(state.context.get(), state.packet.get()) >= 0 &&
                         avcodec_receive_frame(state.context.get(), state.frame.get()) >= 0;
    const auto tookMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    if (!decoded) {
        avcodec_flush_buffers(state.context.get());
        return garbled("a JPEG that cannot be read");
    }
    if (tookMs > decodeBudgetMs) {
        av_frame_unref(state.frame.get());
        return base::Error{"too_slow", "a JPEG that took far too long to decode"};
    }
    const AVFrame& frame = *state.frame;
    const AVPixFmtDescriptor* format = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
    if (frame.width != width || frame.height != height || format == nullptr ||
        (format->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL)) != 0 ||
        format->comp[0].depth != 8 || (format->nb_components != 1 && format->nb_components != 3) ||
        (format->nb_components == 3 && (format->flags & AV_PIX_FMT_FLAG_PLANAR) == 0)) {
        return garbled("a JPEG of a kind this program does not draw");
    }
    // Its brightness and colour as red, green and blue (a JPEG's are of the full range, BT.601), in fixed point.
    const bool grey = format->nb_components == 1;
    const int across = format->log2_chroma_w;
    const int down = format->log2_chroma_h;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(pixels) * 3);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* luma = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        const std::uint8_t* blue =
            grey ? nullptr : frame.data[1] + static_cast<std::ptrdiff_t>(y >> down) * frame.linesize[1];
        const std::uint8_t* red =
            grey ? nullptr : frame.data[2] + static_cast<std::ptrdiff_t>(y >> down) * frame.linesize[2];
        std::uint8_t* to = rgb.data() + static_cast<std::ptrdiff_t>(y) * width * 3;
        for (int x = 0; x < width; ++x, to += 3) {
            const int bright = luma[x] << 16;
            if (grey) {
                to[0] = to[1] = to[2] = luma[x];
                continue;
            }
            const int cb = blue[x >> across] - 128;
            const int cr = red[x >> across] - 128;
            to[0] = clamped((bright + 91881 * cr + 32768) >> 16);
            to[1] = clamped((bright - 22554 * cb - 46802 * cr + 32768) >> 16);
            to[2] = clamped((bright + 116130 * cb + 32768) >> 16);
        }
    }
    av_frame_unref(state.frame.get());
    return rgb;
}

} // namespace qkvm::core
