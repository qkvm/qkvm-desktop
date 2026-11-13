#include "decoder.h"

#include <qkvm/base/h264.h>
#include <qkvm/base/screen_changes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "d3d.h"
#include "gpu_core.h"

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

namespace qkvm::engine {

namespace {

std::atomic<std::uint64_t> ffmpegErrorCount{0};

void countFfmpegErrors(void* /*object*/, int level, const char* /*format*/, va_list /*arguments*/)
{
    if (level <= AV_LOG_ERROR) {
        ffmpegErrorCount.fetch_add(1, std::memory_order_relaxed);
    }
}

using base::h264::ParameterSet;

// Whether an SPS read says a coded picture of a size this program shows.
bool isAShownSize(const ParameterSet& set)
{
    return base::h264::saysASize(set, std::uint64_t{maxPictureWidth}, std::uint64_t{maxPictureHeight});
}

std::optional<ParameterSet> readParameters(std::span<const std::byte> unit, bool sps)
{
    return base::h264::readParameters(unit, sps, std::uint64_t{maxPictureWidth}, std::uint64_t{maxPictureHeight});
}

// Whether an SPS says a coded picture of a size this program shows.
bool spsSaysAShownSize(std::span<const std::byte> unit)
{
    const std::optional<ParameterSet> says = readParameters(unit, true);
    return says && isAShownSize(*says);
}

// What the units of an H.264 packet (Annex B) are, as far as this program judges them before a decoder
// is handed it: its SPS, each by its id, and whether it is sound. It is not where an SPS or a PPS of one
// id comes twice and differs (the same twice is what core's assembler hands over where a device sends
// them in the picture too; two of different ids are lawful, 2d and the SPS gate, after their reading),
// where one cannot be read, or, where `onePicture`, where a second picture begins (a slice whose first
// macroblock is the first) (2c, the second reading: one was judged by its first SPS alone).
struct Units {
    bool sound = true;
    std::vector<std::pair<std::uint32_t, std::span<const std::byte>>> sps;
};
Units unitsOf(std::span<const std::byte> packet, bool onePicture)
{
    Units units;
    std::vector<std::pair<std::uint32_t, std::span<const std::byte>>> pps;
    int pictures = 0;
    for (const std::span<const std::byte> unit : base::h264::units(packet)) {
        const int type = static_cast<std::uint8_t>(unit[0]) & 0x1F;
        const auto same = [](std::span<const std::byte> one, std::span<const std::byte> other) {
            return one.size() == other.size() && std::equal(one.begin(), one.end(), other.begin());
        };
        if (type == 7 || type == 8) {
            auto& sets = type == 7 ? units.sps : pps;
            const std::optional<ParameterSet> read = readParameters(unit, type == 7);
            if (!read) {
                units.sound = false;
            } else if (const auto known = std::ranges::find(
                           sets, read->id, &std::pair<std::uint32_t, std::span<const std::byte>>::first);
                       known == sets.end()) {
                sets.emplace_back(read->id, unit);
            } else {
                units.sound = units.sound && same(known->second, unit);
            }
        } else if ((type == 1 || type == 5) && unit.size() > 1 && (static_cast<std::uint8_t>(unit[1]) & 0x80) != 0) {
            ++pictures;
        }
    }
    units.sound = units.sound && (!onePicture || pictures <= 1);
    return units;
}

// Whether the key frame's parameters say a picture of a size this program shows, as FFmpeg's parser of
// H.264 reads them: it takes nothing by that size, where a decoder takes its tables by it before it
// refuses one too large (2c, after its reading: some 100 MB for an SPS that says 16000 by 16000). One
// whose size is not known (no slice, or nothing the parser could read) is no picture.
bool saysAShownSize(std::span<std::byte> padded, std::size_t bytes)
{
    const ParserPtr parser(av_parser_init(AV_CODEC_ID_H264));
    const CodecContextPtr context(avcodec_alloc_context3(nullptr));
    if (parser == nullptr || context == nullptr || bytes > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    // (The packet is a whole access unit: read at once, not held back for the next one's start.)
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    std::uint8_t* out = nullptr;
    int outSize = 0;
    static_cast<void>(av_parser_parse2(parser.get(), context.get(), &out, &outSize,
                                       reinterpret_cast<const std::uint8_t*>(padded.data()), static_cast<int>(bytes),
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0));
    const auto shown = [](int width, int height) {
        return width > 0 && height > 0 && width <= maxPictureWidth && height <= maxPictureHeight;
    };
    return shown(parser->width, parser->height) && shown(parser->coded_width, parser->coded_height);
}

// Pictures a decoder on the card is asked to keep beyond those it needs itself: the one waiting to
// be drawn, the one being drawn, and one in hand so that the next can be decoded meanwhile.
constexpr int surfacesBesideTheDecoders = 3;

// What a decoder is told of each packet it is handed.
enum class EachPacket : std::uint8_t {
    // Nothing: an H.264 stream says itself where a picture begins that needs no other.
    IsWhatTheStreamSays,
    // That it is a whole picture, which needs no other. A still says nothing of the kind itself,
    // and FFmpeg's decoder of PNGs adds a picture that is not marked so onto the one before it.
    IsAWholePicture,
};

class FfmpegDecoder : public Decoder {
public:
    FfmpegDecoder(CodecContextPtr context, Decoding kind, EachPacket each)
        : m_context(std::move(context)), m_packet(av_packet_alloc()), m_kind(kind), m_each(each)
    {}

    base::Result<Picture> decode(std::span<std::byte> padded, std::size_t bytes, std::int64_t arrivedUs) override
    {
        if (m_packet == nullptr || bytes > padded.size() || padded.size() - bytes < AV_INPUT_BUFFER_PADDING_SIZE) {
            return base::Error{"decoder", "A packet was handed to the decoder without room behind it."};
        }
        if (m_each == EachPacket::IsWhatTheStreamSays) {
            // H.264, live: an SPS new to this decoder is read for its size before the decoder has it,
            // which would take its tables by that size before refusing one too large (2c, the second
            // reading: some 95 MB). The same SPS again, at every key frame, is not read again.
            const Units units = unitsOf(std::span<const std::byte>(padded).first(bytes), false);
            if (!units.sound) {
                return base::Error{std::string(refusedByTheGate), "A picture carries parameters that differ."};
            }
            for (const auto& [id, sps] : units.sps) {
                std::vector<std::byte>& known = m_knownSps[id];
                if (std::ranges::equal(sps, known)) {
                    continue;
                }
                if (!spsSaysAShownSize(sps)) {
                    return base::Error{std::string(refusedByTheGate), "A picture says no size this program shows."};
                }
                known.assign(sps.begin(), sps.end());
            }
        }
        m_packet->data = reinterpret_cast<std::uint8_t*>(padded.data());
        m_packet->size = static_cast<int>(bytes);
        m_packet->flags = m_each == EachPacket::IsAWholePicture ? AV_PKT_FLAG_KEY : 0;
        const int sent = avcodec_send_packet(m_context.get(), m_packet.get());
        m_packet->data = nullptr;
        m_packet->size = 0;
        if (sent < 0) {
            return base::Error{"decoder", "The decoder refused a packet: " + ffmpegSays(sent)};
        }

        Picture picture;
        picture.frame.reset(av_frame_alloc());
        if (picture.frame == nullptr) {
            return base::Error{"memory", "There is no memory for a picture."};
        }
        const int received = avcodec_receive_frame(m_context.get(), picture.frame.get());
        if (received == AVERROR(EAGAIN)) {
            return Picture{};
        }
        if (received < 0) {
            return base::Error{"decoder", "The decoder gave no picture: " + ffmpegSays(received)};
        }
        // What a stream states about itself is not believed before memory is taken for it elsewhere.
        if (picture.frame->width <= 0 || picture.frame->height <= 0 || picture.frame->width > maxPictureWidth ||
            picture.frame->height > maxPictureHeight) {
            return base::Error{"too_large", "A picture is larger than this program shows."};
        }
        picture.arrivedUs = arrivedUs;
        return picture;
    }

    Decoding kind() const noexcept override { return m_kind; }

protected:
    CodecContextPtr m_context;

private:
    PacketPtr m_packet;
    Decoding m_kind;
    EachPacket m_each;
    // The SPS of each id whose size was read and shown (H.264): the same again is not read again.
    std::map<std::uint32_t, std::vector<std::byte>> m_knownSps;
};

// A decoder on the card also counts against the number the Gpu allows.
class CardDecoder final : public FfmpegDecoder {
public:
    CardDecoder(CodecContextPtr context, GpuCore& gpu)
        : FfmpegDecoder(std::move(context), Decoding::OnTheCard, EachPacket::IsWhatTheStreamSays), m_gpu(gpu)
    {}
    ~CardDecoder() override
    {
        // FFmpeg lets go of its surfaces and of its part of the device here.
        m_context.reset();
        m_gpu.giveBackCardDecoder();
    }

private:
    GpuCore& m_gpu;
};

// How a JPEG and a PNG begin (and a whole screen: base/screen_changes.h).
constexpr std::array<std::uint8_t, 2> jpegBegins{0xFF, 0xD8};
constexpr std::array<std::uint8_t, 8> pngBegins{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

template <std::size_t Count>
bool begins(std::span<const std::byte> bytes, const std::array<std::uint8_t, Count>& with)
{
    return bytes.size() >= Count && std::memcmp(bytes.data(), with.data(), Count) == 0;
}

// A value of `depth` bits as one of eight.
std::uint8_t toEight(unsigned value, int depth) noexcept
{
    if (depth >= 8) {
        return static_cast<std::uint8_t>(value >> (depth - 8));
    }
    return static_cast<std::uint8_t>(value * 255U / ((1U << depth) - 1U));
}

// A picture whose colours lie together pixel by pixel, as a PNG's do, as three planes of green,
// blue and red with eight bits each: the one kind of picture with colours of its own that the
// engine draws. A picture in greys becomes three equal planes, and whatever of a picture can be
// seen through is laid over black.
base::Result<Picture> asColourPlanes(Picture packed)
{
    const AVFrame& from = *packed.frame;
    const AVPixFmtDescriptor* said = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(from.format));
    const std::uint64_t unwanted = AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT;
    if (said == nullptr || (said->flags & unwanted) != 0 || from.data[0] == nullptr) {
        return base::Error{"decoder", "A picture came in a form this program does not draw."};
    }
    const bool palette = (said->flags & AV_PIX_FMT_FLAG_PAL) != 0;
    const bool colour = (said->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const bool seenThrough = !palette && (said->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    // Red, green, blue and what is seen through, as FFmpeg numbers them: a picture in greys has its
    // one value first and what is seen through second.
    const int alphaAt = colour ? 3 : 1;
    if (palette ? from.data[1] == nullptr : said->nb_components != (colour ? 3 : 1) + (seenThrough ? 1 : 0)) {
        return base::Error{"decoder", "A picture came in a form this program does not draw."};
    }

    Picture planes;
    planes.arrivedUs = packed.arrivedUs;
    planes.frame.reset(av_frame_alloc());
    if (planes.frame == nullptr) {
        return base::Error{"memory", "There is no memory for a picture."};
    }
    planes.frame->format = AV_PIX_FMT_GBRP;
    planes.frame->width = from.width;
    planes.frame->height = from.height;
    if (av_frame_get_buffer(planes.frame.get(), 0) < 0) {
        return base::Error{"memory", "There is no memory for a picture."};
    }

    const auto width = static_cast<std::size_t>(from.width);
    std::vector<std::uint16_t> read[4];
    for (std::vector<std::uint16_t>& line : read) {
        line.resize(width);
    }
    for (int y = 0; y < from.height; ++y) {
        std::uint8_t* const green = planes.frame->data[0] + static_cast<std::ptrdiff_t>(y) * planes.frame->linesize[0];
        std::uint8_t* const blue = planes.frame->data[1] + static_cast<std::ptrdiff_t>(y) * planes.frame->linesize[1];
        std::uint8_t* const red = planes.frame->data[2] + static_cast<std::ptrdiff_t>(y) * planes.frame->linesize[2];
        if (palette) {
            // Each pixel is a number into a list of 256 colours, each of them alpha, red, green and
            // blue in one 32-bit number.
            const std::uint8_t* const numbers = from.data[0] + static_cast<std::ptrdiff_t>(y) * from.linesize[0];
            for (std::size_t x = 0; x < width; ++x) {
                std::uint32_t entry = 0;
                std::memcpy(&entry, from.data[1] + std::size_t{4} * numbers[x], sizeof(entry));
                const unsigned alpha = entry >> 24;
                red[x] = static_cast<std::uint8_t>(((entry >> 16) & 0xFFU) * alpha / 255U);
                green[x] = static_cast<std::uint8_t>(((entry >> 8) & 0xFFU) * alpha / 255U);
                blue[x] = static_cast<std::uint8_t>((entry & 0xFFU) * alpha / 255U);
            }
            continue;
        }
        const auto readLine = [&](int component, std::vector<std::uint16_t>& into) {
            av_read_image_line2(into.data(), const_cast<const std::uint8_t**>(from.data), from.linesize, said, 0, y,
                                component, from.width, 0, sizeof(std::uint16_t));
        };
        readLine(0, read[0]);
        if (colour) {
            readLine(1, read[1]);
            readLine(2, read[2]);
        }
        if (seenThrough) {
            readLine(alphaAt, read[3]);
        }
        const int depth = said->comp[0].depth;
        const int alphaDepth = said->comp[alphaAt].depth;
        for (std::size_t x = 0; x < width; ++x) {
            const unsigned alpha = seenThrough ? toEight(read[3][x], alphaDepth) : 255U;
            const unsigned first = toEight(read[0][x], depth) * alpha / 255U;
            red[x] = static_cast<std::uint8_t>(first);
            green[x] = colour ? static_cast<std::uint8_t>(toEight(read[1][x], depth) * alpha / 255U)
                              : static_cast<std::uint8_t>(first);
            blue[x] = colour ? static_cast<std::uint8_t>(toEight(read[2][x], depth) * alpha / 255U)
                             : static_cast<std::uint8_t>(first);
        }
    }
    return planes;
}

AVPixelFormat pickTheCard(AVCodecContext* /*context*/, const AVPixelFormat* offered)
{
    for (const AVPixelFormat* format = offered; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_D3D11) {
            return AV_PIX_FMT_D3D11;
        }
    }
    // Never a software format by the back door: whoever asked for the card is told it did not work,
    // and decides what to do instead.
    return AV_PIX_FMT_NONE;
}

base::Result<CodecContextPtr> openContext(AVCodecID id, BufferPtr device)
{
    const AVCodec* codec = avcodec_find_decoder(id);
    if (codec == nullptr) {
        return base::Error{"decoder", "This build of FFmpeg has no decoder for this kind of picture."};
    }
    CodecContextPtr context(avcodec_alloc_context3(codec));
    if (context == nullptr) {
        return base::Error{"memory", "There is no memory for a decoder."};
    }
    // What a picture states about its own size is not believed: one larger than the program shows
    // is refused by the decoder before it takes memory for it.
    context->max_pixels = std::int64_t{maxPictureWidth} * maxPictureHeight;
    // A KVM's pictures are shown the moment they are decoded: nothing is held back to be put in
    // order, and no second thread decodes ahead, which would delay each picture by one.
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->thread_count = 1;
    if (device != nullptr) {
        context->hw_device_ctx = device.release();
        context->get_format = pickTheCard;
        context->extra_hw_frames = surfacesBesideTheDecoders;
    }
    const int opened = avcodec_open2(context.get(), codec, nullptr);
    if (opened < 0) {
        return base::Error{"decoder", "The decoder could not be started: " + ffmpegSays(opened)};
    }
    return context;
}

// How a key frame of H.264 begins as a JetKVM's connection hands one over: a start code and its SPS
// (core's h264_rtp.cpp puts the SPS and PPS in front of every key frame).
constexpr std::array<std::uint8_t, 4> startCode{0, 0, 0, 1};
bool beginsAKeyFrame(std::span<const std::byte> bytes)
{
    return bytes.size() > startCode.size() && begins(bytes, startCode) &&
           (static_cast<std::uint8_t>(bytes[startCode.size()]) & 0x1F) == 7;
}

// A key frame of H.264 as a still (design 4, D-e): decoded by a decoder made for it alone, given the
// packet and then the stream's end, its one picture taken in software planes, and the decoder freed.
// Nothing stays between pictures: no picture of one card can come out for another (a decoder's reorder
// delay), 64 cards hold no 10 MB decoder each, and a broken key frame breaks that one picture.
base::Result<Picture> keyFrameByItself(std::span<std::byte> padded, std::size_t bytes, std::int64_t arrivedUs)
{
    if (const Units units = unitsOf(std::span<const std::byte>(padded).first(bytes), true);
        !units.sound || units.sps.empty() ||
        !std::ranges::all_of(units.sps, [](const auto& sps) { return spsSaysAShownSize(sps.second); })) {
        return base::Error{std::string(refusedByTheGate),
                           "A key frame carries more than one picture, parameters that differ, or too large a size."};
    }
    if (!saysAShownSize(padded, bytes)) {
        return base::Error{"too_large", "A key frame says no picture of a size this program shows."};
    }
    base::Result<CodecContextPtr> context = openContext(AV_CODEC_ID_H264, nullptr);
    if (!context) {
        return context.error();
    }
    const PacketPtr packet(av_packet_alloc());
    Picture picture;
    picture.frame.reset(av_frame_alloc());
    if (packet == nullptr || picture.frame == nullptr) {
        return base::Error{"memory", "There is no memory for a picture."};
    }
    packet->data = reinterpret_cast<std::uint8_t*>(padded.data());
    packet->size = static_cast<int>(bytes);
    packet->flags = AV_PKT_FLAG_KEY;
    const int sent = avcodec_send_packet(context.value().get(), packet.get());
    packet->data = nullptr;
    packet->size = 0;
    if (sent < 0) {
        return base::Error{"decoder", "The decoder refused a key frame: " + ffmpegSays(sent)};
    }
    // The end of the stream: whatever the decoder holds back comes out now.
    static_cast<void>(avcodec_send_packet(context.value().get(), nullptr));
    const int received = avcodec_receive_frame(context.value().get(), picture.frame.get());
    if (received < 0) {
        return base::Error{"decoder", "A key frame gave no picture: " + ffmpegSays(received)};
    }
    if (picture.frame->width <= 0 || picture.frame->height <= 0 || picture.frame->width > maxPictureWidth ||
        picture.frame->height > maxPictureHeight) {
        return base::Error{"too_large", "A picture is larger than this program shows."};
    }
    picture.arrivedUs = arrivedUs;
    return picture;
}

// Stills: a JPEG, a PNG or a key frame of H.264, told apart by how each packet begins. A decoder for
// each of the first two is kept from when it is first needed: a KVM without a signal sends a PNG
// where it otherwise sends JPEGs. A key frame has a decoder of its own each time (keyFrameByItself).
class StillDecoder final : public Decoder {
public:
    // Key frames only where the stream was made for them (Codec::KeyFrameStill): its source says it
    // sends them, and how a packet begins never decides that (2c, after its reading).
    explicit StillDecoder(bool keyFrames) : m_keyFrames(keyFrames) {}

    base::Result<Picture> decode(std::span<std::byte> padded, std::size_t bytes, std::int64_t arrivedUs) override
    {
        const std::span<const std::byte> packet =
            std::span<const std::byte>(padded).first(std::min(bytes, padded.size()));
        if (m_keyFrames && beginsAKeyFrame(packet)) {
            if (bytes > padded.size() || padded.size() - bytes < AV_INPUT_BUFFER_PADDING_SIZE) {
                return base::Error{"decoder", "A packet was handed to the decoder without room behind it."};
            }
            return keyFrameByItself(padded, bytes, arrivedUs);
        }
        if (begins(packet, base::screen::mark)) {
            return wholeScreen(packet, arrivedUs);
        }
        const bool png = begins(packet, pngBegins);
        if (!png && !begins(packet, jpegBegins)) {
            return base::Error{"decoder", m_keyFrames ? "What came is neither a JPEG, a PNG nor a key frame of H.264."
                                                      : "What came is neither a JPEG nor a PNG."};
        }
        std::unique_ptr<FfmpegDecoder>& decoder = png ? m_png : m_jpeg;
        if (decoder == nullptr) {
            base::Result<CodecContextPtr> context = openContext(png ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG, nullptr);
            if (!context) {
                return context.error();
            }
            decoder = std::make_unique<FfmpegDecoder>(std::move(context).value(), Decoding::InSoftware,
                                                      EachPacket::IsAWholePicture);
        }
        base::Result<Picture> picture = decoder->decode(padded, bytes, arrivedUs);
        if (!png || !picture || !picture.value()) {
            return picture;
        }
        return asColourPlanes(std::move(picture).value());
    }

    Decoding kind() const noexcept override { return Decoding::InSoftware; }

private:
    // A whole screen (a server's, base/screen_changes.h) as the three planes of green, blue and red a picture with
    // colours of its own is drawn from. What changed of a screen is no still: a still is the whole of it.
    static base::Result<Picture> wholeScreen(std::span<const std::byte> packet, std::int64_t arrivedUs)
    {
        const std::optional<base::screen::Changes> read = base::screen::read(packet);
        if (!read || !read->whole()) {
            return base::Error{"decoder", "What came is not a whole screen: a still is the whole of one."};
        }
        Picture picture;
        picture.arrivedUs = arrivedUs;
        picture.frame.reset(av_frame_alloc());
        if (picture.frame == nullptr) {
            return base::Error{"memory", "There is no memory for a picture."};
        }
        picture.frame->format = AV_PIX_FMT_GBRP;
        picture.frame->width = read->width;
        picture.frame->height = read->height;
        if (av_frame_get_buffer(picture.frame.get(), 0) < 0) {
            return base::Error{"memory", "There is no memory for a picture."};
        }
        const std::byte* from = read->pixels.data();
        for (int y = 0; y < read->height; ++y) {
            AVFrame& frame = *picture.frame;
            std::uint8_t* const green = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
            std::uint8_t* const blue = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
            std::uint8_t* const red = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
            for (int x = 0; x < read->width; ++x) {
                red[x] = static_cast<std::uint8_t>(from[0]);
                green[x] = static_cast<std::uint8_t>(from[1]);
                blue[x] = static_cast<std::uint8_t>(from[2]);
                from += 3;
            }
        }
        return picture;
    }

    bool m_keyFrames;
    std::unique_ptr<FfmpegDecoder> m_jpeg;
    std::unique_ptr<FfmpegDecoder> m_png;
};

} // namespace

std::string ffmpegSays(int result)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, text, sizeof(text));
    return text;
}

void quietFfmpeg()
{
    av_log_set_callback(countFfmpegErrors);
}

std::uint64_t ffmpegErrors() noexcept
{
    return ffmpegErrorCount.load(std::memory_order_relaxed);
}

base::Result<std::unique_ptr<Decoder>> makeSoftwareDecoder(Codec codec)
{
    if (codec != Codec::H264) {
        return std::unique_ptr<Decoder>(std::make_unique<StillDecoder>(codec == Codec::KeyFrameStill));
    }
    base::Result<CodecContextPtr> context = openContext(AV_CODEC_ID_H264, nullptr);
    if (!context) {
        return context.error();
    }
    return std::unique_ptr<Decoder>(std::make_unique<FfmpegDecoder>(std::move(context).value(), Decoding::InSoftware,
                                                                    EachPacket::IsWhatTheStreamSays));
}

base::Result<std::unique_ptr<Decoder>> makeCardDecoder(GpuCore& gpu)
{
    if (!gpu.takeCardDecoder()) {
        return base::Error{"card_full", "The graphics card is decoding as many streams as it is given."};
    }
    BufferPtr device = gpu.decodingDevice();
    if (device == nullptr) {
        gpu.giveBackCardDecoder();
        return base::Error{"no_card", "This graphics device cannot decode video."};
    }
    base::Result<CodecContextPtr> context = openContext(AV_CODEC_ID_H264, std::move(device));
    if (!context) {
        gpu.giveBackCardDecoder();
        return context.error();
    }
    return std::unique_ptr<Decoder>(std::make_unique<CardDecoder>(std::move(context).value(), gpu));
}

} // namespace qkvm::engine
