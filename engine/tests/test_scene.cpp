#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Windows itself, for how much memory the process was given; the first of the two has to come first.
#include <windows.h>

#include <dxgi1_6.h>
#include <psapi.h>
#include <wrl/client.h>

#include <QElapsedTimer>
#include <QTest>

#include <qkvm/base/clock.h>
#include <qkvm/base/screen_changes.h>
#include <qkvm/engine/gpu.h>
#include <qkvm/engine/scene.h>

#include "clips.h"

// How many errors FFmpeg has said in this process, as the engine counts them (engine/src/ffmpeg.h, whose
// FFmpeg headers are the engine's alone): what FFmpeg was never handed, it says nothing of.
namespace qkvm::engine {
std::uint64_t ffmpegErrors() noexcept;
} // namespace qkvm::engine

using namespace qkvm;

// The engine on Windows' software adapter, which every PC has: packets in, a picture drawn into
// memory, and its colours looked at. The pictures are the ones tools/make_testdata.ps1 makes, and
// their colours are known because that script chose them.
class TestScene : public QObject {
    Q_OBJECT

private:
    struct Colour {
        int red = 0;
        int green = 0;
        int blue = 0;
    };
    // The "desktop" of the made pictures.
    static constexpr Colour ground{0x0f, 0x17, 0x2a};
    static constexpr Colour panel{0x1e, 0x29, 0x3b};
    static constexpr Colour titleBar{0x1d, 0x4e, 0xd8};
    // The three strong colours: read in the wrong colour standard they come out far from this.
    static constexpr Colour red{0xdc, 0x26, 0x26};
    static constexpr Colour green{0x22, 0xc5, 0x5e};
    static constexpr Colour amber{0xf5, 0x9e, 0x0b};
    static constexpr Colour black{0, 0, 0};

    // What one of the made PNGs holds, against the desktop's own colours.
    enum Holds { Colours, HalfTheColours, Greys, BlackAndWhite };

    static std::filesystem::path made(const char* name) { return std::filesystem::path(QKVM_TESTDATA) / name; }

    // The most memory this process has ever had committed to it, as Windows counts it.
    static std::size_t mostEverCommitted()
    {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0 ? counters.PeakPagefileUsage
                                                                                           : 0;
    }

    // The beginning of a PNG that says it is this large, with a little that is no picture behind it:
    // the head, with its check sum right, and the start of the picture's data.
    static std::vector<std::byte> pngThatClaims(std::uint32_t width, std::uint32_t height)
    {
        std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        const auto number = [&png](std::uint32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                png.push_back(static_cast<std::uint8_t>(value >> shift));
            }
        };
        const auto part = [&](const char* name, const std::vector<std::uint8_t>& content) {
            number(static_cast<std::uint32_t>(content.size()));
            const std::size_t from = png.size();
            png.insert(png.end(), name, name + 4);
            png.insert(png.end(), content.begin(), content.end());
            // The check sum every part of a PNG ends with, over its name and its content.
            std::uint32_t sum = 0xFFFFFFFFU;
            for (std::size_t at = from; at < png.size(); ++at) {
                sum ^= png[at];
                for (int bit = 0; bit < 8; ++bit) {
                    sum = (sum >> 1) ^ ((sum & 1U) != 0 ? 0xEDB88320U : 0U);
                }
            }
            number(~sum);
        };
        std::vector<std::uint8_t> head;
        for (const std::uint32_t side : {width, height}) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                head.push_back(static_cast<std::uint8_t>(side >> shift));
            }
        }
        // Eight bits a value; red, green and blue; and the three settings that are always nought.
        head.insert(head.end(), {8, 2, 0, 0, 0});
        part("IHDR", head);
        part("IDAT", {0x78, 0x9C, 0x01, 0x02});
        std::vector<std::byte> bytes(png.size());
        std::memcpy(bytes.data(), png.data(), png.size());
        return bytes;
    }

    // A device that checks every call where Windows has the tools for that, and one that does not
    // where it has not: the tests of what is drawn hold either way.
    static std::unique_ptr<engine::Gpu> softwareGpu(int decodeThreads = 0)
    {
        for (const bool checked : {true, false}) {
            engine::Gpu::Options options;
            options.software = true;
            options.checked = checked;
            options.decodeThreads = decodeThreads;
            if (base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create(options)) {
                return std::move(made).value();
            }
        }
        return nullptr;
    }

    static Colour colourAt(const std::vector<std::uint8_t>& pixels, engine::SizePx size, int x, int y)
    {
        const std::size_t at =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width) + static_cast<std::size_t>(x)) * 4;
        return {pixels[at + 2], pixels[at + 1], pixels[at]};
    }

    // Lossy pictures, and a colour that went through two colour spaces: close to it is the same.
    static bool closeTo(Colour is, Colour wanted, int within)
    {
        return std::abs(is.red - wanted.red) <= within && std::abs(is.green - wanted.green) <= within &&
               std::abs(is.blue - wanted.blue) <= within;
    }

    static QByteArray said(Colour is, Colour wanted)
    {
        return QByteArray("is ") + QByteArray::number(is.red) + ',' + QByteArray::number(is.green) + ',' +
               QByteArray::number(is.blue) + ", should be " + QByteArray::number(wanted.red) + ',' +
               QByteArray::number(wanted.green) + ',' + QByteArray::number(wanted.blue);
    }

#define COMPARE_COLOUR(is, wanted, within)                                                                             \
    do {                                                                                                               \
        const Colour isNow = (is);                                                                                     \
        QVERIFY2(closeTo(isNow, (wanted), (within)), said(isNow, (wanted)).constData());                               \
    } while (false)

    // What Direct3D's checking objected to since it was last asked, a line each. Asking empties it.
    static QByteArray objectionsOf(engine::Gpu& gpu)
    {
        QByteArray all;
        for (const std::string& objection : gpu.objections()) {
            all += QByteArray::fromStdString(objection) + '\n';
        }
        return all;
    }

#define VERIFY_NO_OBJECTIONS(gpu)                                                                                      \
    do {                                                                                                               \
        const QByteArray objections = objectionsOf(gpu);                                                               \
        QVERIFY2(objections.isEmpty(), objections.constData());                                                        \
    } while (false)

    static void pushFile(engine::Stream& stream, const std::filesystem::path& file)
    {
        const base::Result<std::vector<std::byte>> bytes = bench::readFile(file);
        QVERIFY2(bytes.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        stream.push({bytes.value(), base::steadyNowUs(), true});
    }

    // The units of an Annex B stream: where each one's start code begins (three bytes or four), and
    // what kind it is (5 a key frame's slice, 7 an SPS, 9 a delimiter).
    struct Unit {
        std::size_t start = 0;
        std::size_t header = 0;
        int type = 0;
    };
    static std::vector<Unit> unitsOf(std::span<const std::byte> bytes)
    {
        std::vector<Unit> units;
        for (std::size_t at = 0; at + 3 < bytes.size(); ++at) {
            if (bytes[at] == std::byte{0} && bytes[at + 1] == std::byte{0} && bytes[at + 2] == std::byte{1}) {
                const std::size_t start = at > 0 && bytes[at - 1] == std::byte{0} ? at - 1 : at;
                units.push_back({start, at + 3, static_cast<int>(static_cast<std::uint8_t>(bytes[at + 3]) & 0x1F)});
                at += 2;
            }
        }
        return units;
    }

    // The `nth` key frame of a made clip as the WebRTC assembler hands one over (core's h264_rtp.cpp):
    // a four-byte start code and its SPS first. (The made clips put an access unit delimiter before
    // every picture.)
    static std::vector<std::byte> keyFrameOf(const char* clip, std::size_t nth)
    {
        const base::Result<std::vector<std::byte>> bytes = bench::readFile(made(clip));
        if (!bytes) {
            return {};
        }
        const std::span<const std::byte> all(bytes.value());
        std::size_t seen = 0;
        for (const bench::Cut& cut : bench::cutIntoPictures(all)) {
            if (!cut.keyFrame || seen++ != nth) {
                continue;
            }
            const std::span<const std::byte> picture = all.subspan(cut.from, cut.size);
            for (const Unit& unit : unitsOf(picture)) {
                if (unit.type == 7) {
                    std::vector<std::byte> key{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
                    key.insert(key.end(), picture.begin() + static_cast<std::ptrdiff_t>(unit.header), picture.end());
                    return key;
                }
            }
        }
        return {};
    }

    // A key frame without its picture: the SPS and PPS before its first slice, and nothing more.
    static std::vector<std::byte> headsOnly(const std::vector<std::byte>& keyFrame)
    {
        for (const Unit& unit : unitsOf(keyFrame)) {
            if (unit.type == 5) {
                return {keyFrame.begin(), keyFrame.begin() + static_cast<std::ptrdiff_t>(unit.start)};
            }
        }
        return {};
    }

    // A number as H.264's ue(v) writes it, as '0' and '1'.
    static std::string ueBits(unsigned value)
    {
        int length = 0;
        while (((value + 1) >> (length + 1)) != 0) {
            ++length;
        }
        std::string bits(static_cast<std::size_t>(length), '0');
        for (int at = length; at >= 0; --at) {
            bits += (((value + 1) >> at) & 1U) != 0 ? '1' : '0';
        }
        return bits;
    }

    // An SPS, a PPS and the beginning of an IDR slice that say the picture is `widthMbs` by `heightMbs`
    // macroblocks of 16 pixels: baseline, level 5.1, nothing more in them, a few bytes in all.
    // `profile`: another profile_idc than baseline's, with its chroma fields where the book has them for it
    // (4:2:0, 8 bits, no scaling lists). `spsBits`: where given, the SPS after its level is these bits as they
    // stand (its id first), and the slice has a picture order count of 4 bits (as such an SPS of the tests
    // says); `widthMbs`, `heightMbs` and `spsId` are not used then.
    static std::vector<std::byte> keyFrameThatSays(unsigned widthMbs, unsigned heightMbs, unsigned spsId = 0,
                                                   unsigned ppsId = 0, unsigned profile = 66,
                                                   std::string_view spsBits = {})
    {
        struct Bits {
            std::vector<std::uint8_t> bytes;
            int free = 0;
            void bit(unsigned one)
            {
                if (free == 0) {
                    bytes.push_back(0);
                    free = 8;
                }
                --free;
                bytes.back() = static_cast<std::uint8_t>(bytes.back() | ((one & 1U) << free));
            }
            void bits(unsigned value, int count)
            {
                for (int at = count - 1; at >= 0; --at) {
                    bit((value >> at) & 1U);
                }
            }
            void ue(unsigned value)
            {
                int length = 0;
                while (((value + 1) >> (length + 1)) != 0) {
                    ++length;
                }
                bits(0, length);
                bits(value + 1, length + 1);
            }
            void end()
            {
                bit(1);
                free = 0;
            }
        };
        std::vector<std::byte> key;
        const auto unit = [&key](std::uint8_t header, const Bits& body) {
            for (const std::uint8_t byte :
                 {std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, header}) {
                key.push_back(static_cast<std::byte>(byte));
            }
            int zeroes = 0;
            for (const std::uint8_t byte : body.bytes) {
                if (zeroes >= 2 && byte <= 3) {
                    key.push_back(std::byte{3});
                    zeroes = 0;
                }
                key.push_back(static_cast<std::byte>(byte));
                zeroes = byte == 0 ? zeroes + 1 : 0;
            }
        };
        Bits sps;
        sps.bits(profile, 8);
        sps.bits(profile == 66 ? 0xC0 : 0, 8); // constraint sets 0 and 1 (baseline's)
        sps.bits(51, 8);                       // level 5.1
        if (!spsBits.empty()) {
            for (const char bit : spsBits) {
                sps.bit(bit == '1' ? 1 : 0);
            }
        } else {
            sps.ue(spsId); // seq_parameter_set_id
            if (profile != 66 && profile != 77 && profile != 88) {
                sps.ue(1);  // chroma_format_idc: 4:2:0
                sps.ue(0);  // bit_depth_luma_minus8
                sps.ue(0);  // bit_depth_chroma_minus8
                sps.bit(0); // qpprime_y_zero_transform_bypass_flag
                sps.bit(0); // seq_scaling_matrix_present_flag
            }
            sps.ue(0);  // log2_max_frame_num_minus4
            sps.ue(2);  // pic_order_cnt_type
            sps.ue(1);  // max_num_ref_frames
            sps.bit(0); // gaps_in_frame_num_value_allowed_flag
            sps.ue(widthMbs - 1);
            sps.ue(heightMbs - 1);
            sps.bit(1); // frame_mbs_only_flag
            sps.bit(1); // direct_8x8_inference_flag
            sps.bit(0); // frame_cropping_flag
            sps.bit(0); // vui_parameters_present_flag
        }
        sps.end();
        unit(0x67, sps);
        Bits pps;
        pps.ue(ppsId);  // pic_parameter_set_id
        pps.ue(spsId);  // seq_parameter_set_id
        pps.bit(0);     // entropy_coding_mode_flag
        pps.bit(0);     // bottom_field_pic_order_in_frame_present_flag
        pps.ue(0);      // num_slice_groups_minus1
        pps.ue(0);      // num_ref_idx_l0_default_active_minus1
        pps.ue(0);      // num_ref_idx_l1_default_active_minus1
        pps.bit(0);     // weighted_pred_flag
        pps.bits(0, 2); // weighted_bipred_idc
        pps.ue(0);      // pic_init_qp_minus26 (as se: 0)
        pps.ue(0);      // pic_init_qs_minus26
        pps.ue(0);      // chroma_qp_index_offset
        pps.bit(1);     // deblocking_filter_control_present_flag
        pps.bit(0);     // constrained_intra_pred_flag
        pps.bit(0);     // redundant_pic_cnt_present_flag
        pps.end();
        unit(0x68, pps);
        Bits slice;
        slice.ue(0);      // first_mb_in_slice
        slice.ue(7);      // slice_type: I, and all of the picture's
        slice.ue(ppsId);  // pic_parameter_set_id
        slice.bits(0, 4); // frame_num
        slice.ue(0);      // idr_pic_id
        if (!spsBits.empty()) {
            slice.bits(0, 4); // pic_order_cnt_lsb
        }
        slice.bit(0); // no_output_of_prior_pics_flag
        slice.bit(0); // long_term_reference_flag
        slice.ue(0);  // slice_qp_delta (as se: 0)
        slice.ue(1);  // disable_deblocking_filter_idc
        slice.bits(0x5A5A5A, 24);
        slice.end();
        unit(0x65, slice);
        return key;
    }

    // A server's screen, three bytes a pixel: the left half one colour and the right half another.
    static std::vector<std::uint8_t> halves(int width, int height, Colour left, Colour right)
    {
        std::vector<std::uint8_t> rgb;
        rgb.reserve(std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const Colour& colour = x < width / 2 ? left : right;
                rgb.insert(rgb.end(), {static_cast<std::uint8_t>(colour.red), static_cast<std::uint8_t>(colour.green),
                                       static_cast<std::uint8_t>(colour.blue)});
            }
        }
        return rgb;
    }
    // The same screen with these rectangles of it in one colour, and the packet of what changed.
    static std::vector<std::byte> painted(std::vector<std::uint8_t>& rgb, int width, int height,
                                          const std::vector<base::screen::Rect>& rects, Colour colour)
    {
        for (const base::screen::Rect& rect : rects) {
            for (int y = rect.y; y < rect.y + rect.height; ++y) {
                for (int x = rect.x; x < rect.x + rect.width; ++x) {
                    const std::size_t at =
                        std::size_t{3} *
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x));
                    rgb[at] = static_cast<std::uint8_t>(colour.red);
                    rgb[at + 1] = static_cast<std::uint8_t>(colour.green);
                    rgb[at + 2] = static_cast<std::uint8_t>(colour.blue);
                }
            }
        }
        return base::screen::write(width, height, rgb, rects);
    }
    static void pushBytes(engine::Stream& stream, const std::vector<std::byte>& bytes)
    {
        stream.push({bytes, base::steadyNowUs(), false});
    }
    // What a scene draws of one stream into a canvas of that size: the pixels, or nothing.
    static std::vector<std::uint8_t> drawnInto(engine::Scene& scene, engine::Canvas& canvas)
    {
        if (!scene.draw(canvas.target())) {
            return {};
        }
        base::Result<std::vector<std::uint8_t>> pixels = canvas.pixels();
        return pixels ? std::move(pixels).value() : std::vector<std::uint8_t>{};
    }

    // The made PNG with a note in it after its head (a text part, which says nothing of the picture):
    // the same picture in other bytes.
    static std::vector<std::byte> pngWithANote(const std::filesystem::path& file, int note)
    {
        const base::Result<std::vector<std::byte>> bytes = bench::readFile(file);
        if (!bytes || bytes.value().size() < 33) {
            return {};
        }
        std::vector<std::uint8_t> part{0, 0, 0, 0, 't', 'E', 'X', 't', 'n', 'o', 't', 'e', 0};
        part.push_back(static_cast<std::uint8_t>('0' + note % 10));
        part[3] = static_cast<std::uint8_t>(part.size() - 8);
        std::uint32_t sum = 0xFFFFFFFFU;
        for (std::size_t at = 4; at < part.size(); ++at) {
            sum ^= part[at];
            for (int bit = 0; bit < 8; ++bit) {
                sum = (sum >> 1) ^ ((sum & 1U) != 0 ? 0xEDB88320U : 0U);
            }
        }
        sum = ~sum;
        for (int shift = 24; shift >= 0; shift -= 8) {
            part.push_back(static_cast<std::uint8_t>(sum >> shift));
        }
        // The signature (8) and the head (25) first: a text part may stand anywhere after the head.
        std::vector<std::byte> noted(bytes.value().begin(), bytes.value().begin() + 33);
        for (const std::uint8_t byte : part) {
            noted.push_back(static_cast<std::byte>(byte));
        }
        noted.insert(noted.end(), bytes.value().begin() + 33, bytes.value().end());
        return noted;
    }

private slots:
    // 2c, after its reading (M2): a key frame whose SPS says its picture is 16000 by 16000 reaches no
    // decoder, which would take its tables by that size before refusing it (some 100 MB a still): the
    // size its parameters say is read first, by FFmpeg's parser, which takes nothing by it. And one that
    // says a picture of a size the program shows is decoded. First of the tests, so that the process's
    // most memory ever committed says something.
    void aKeyFrameThatSaysItIsHugeReachesNoDecoder()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::KeyFrameStill);
        // (One that is decoded first, so that what FFmpeg makes once is made before the count begins.)
        stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        const std::size_t before = mostEverCommitted();
        stream.push({keyFrameThatSays(1000, 1000), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        const std::size_t after = mostEverCommitted();
        qInfo() << "most committed grew by" << (after - before) / 1024 << "KB";
        QVERIFY2(
            after - before < std::size_t{16} * 1024 * 1024,
            qPrintable(QStringLiteral("%1 MB were taken for it").arg((after - before) / (std::size_t{1024} * 1024))));
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
    }

    // 2c, the second reading (L2): a live H.264 stream's decoder, in software or on the card, is handed
    // no SPS that says a picture larger than the program shows: each SPS that is new to it is read for
    // its size first, as a key-frame still's is, and the packet is refused (the stream waits for its
    // next key frame). Before: some 95 MB were taken before the decoder refused it. Second of the tests,
    // for the process's most memory ever committed.
    void aLiveStreamThatSaysItIsHugeReachesNoDecoder()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::H264);
        stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        const std::size_t before = mostEverCommitted();
        stream.push({keyFrameThatSays(1000, 1000), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        const std::size_t after = mostEverCommitted();
        qInfo() << "most committed grew by" << (after - before) / 1024 << "KB";
        QVERIFY2(
            after - before < std::size_t{16} * 1024 * 1024,
            qPrintable(QStringLiteral("%1 MB were taken for it").arg((after - before) / (std::size_t{1024} * 1024))));
        // And it goes on at its next key frame that is one.
        stream.push({keyFrameOf("desktop-720p15.h264", 1), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
    }

    // 2e and the SPS reader, after their reading (M2): the program's reading of an SPS and FFmpeg's may differ
    // where they differ on which profiles carry chroma fields (144, an old one, and 139, 134, 135). An SPS of
    // profile 144 is made that says 16 by 64 to a reader without them and 16000 by 16000 to one with them
    // (found by a short search: the commit's message). Only profiles that both read alike are taken (66, 77,
    // 88, 100, 110, 122, 244): it is refused before any decoder has it, live or as a still, and takes no
    // memory by its size. (Before: the live gate read 16 by 64 and handed it to the decoder.) Third of the
    // tests, for the process's most memory ever committed.
    void anSpsOfAProfileReadTwoWaysReachesNoDecoder()
    {
        constexpr std::string_view twoWays = "1111001110101000000000111110100000000000011111010001100";
        for (const engine::Codec codec : {engine::Codec::H264, engine::Codec::KeyFrameStill}) {
            const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
            QVERIFY(gpu != nullptr);
            const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
            engine::Stream& stream = scene->add(1, codec);
            stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
            const std::size_t before = mostEverCommitted();
            stream.push({keyFrameThatSays(1, 1, 0, 0, 144, twoWays), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
            const std::size_t after = mostEverCommitted();
            qInfo() << (codec == engine::Codec::H264 ? "live:" : "still:") << "most committed grew by"
                    << (after - before) / 1024 << "KB";
            QVERIFY2(after - before < std::size_t{16} * 1024 * 1024,
                     qPrintable(
                         QStringLiteral("%1 MB were taken for it").arg((after - before) / (std::size_t{1024} * 1024))));
            // And it goes on at its next key frame that is one.
            stream.push({keyFrameOf("desktop-720p15.h264", 1), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        }
    }

    // 2e, the second reading (M2): where FFmpeg's first reading of an SPS fails, it reads it a second way, as it
    // came (its emulation bytes left in). So what it refuses on that first reading is refused here before any
    // of FFmpeg has it, live or as a still: more than 16 reference frames, two bit depths, colour planes coded
    // apart. FFmpeg says nothing of it (before: it said its errors, and the packet reached it).
    void whatFfmpegRefusesReachesNoDecoder_data()
    {
        QTest::addColumn<int>("profile");
        QTest::addColumn<QByteArray>("bits");
        // (After the id and the chroma fields: frame numbers and picture order counts of 4 bits, as the slice
        // of keyFrameThatSays has them; one reference frame, or 17; 1280 by 720, frames only, no cropping, no
        // VUI.)
        const auto rest = [](unsigned references) {
            return ueBits(0) + ueBits(0) + ueBits(0) + ueBits(references) + "0" + ueBits(79) + ueBits(44) + "1100";
        };
        QTest::newRow("17 reference frames") << 66 << QByteArray::fromStdString(ueBits(0) + rest(17));
        QTest::newRow("two bit depths") << 100
                                        << QByteArray::fromStdString(ueBits(0) + ueBits(1) + ueBits(0) + ueBits(1) +
                                                                     "00" + rest(1));
        QTest::newRow("colour planes apart")
            << 244 << QByteArray::fromStdString(ueBits(0) + ueBits(3) + "1" + ueBits(0) + ueBits(0) + "00" + rest(1));
    }
    void whatFfmpegRefusesReachesNoDecoder()
    {
        QFETCH(int, profile);
        QFETCH(QByteArray, bits);
        for (const engine::Codec codec : {engine::Codec::H264, engine::Codec::KeyFrameStill}) {
            const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
            QVERIFY(gpu != nullptr);
            const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
            engine::Stream& stream = scene->add(1, codec);
            stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
            const std::uint64_t errors = engine::ffmpegErrors();
            stream.push({keyFrameThatSays(1, 1, 0, 0, static_cast<unsigned>(profile), bits.toStdString()),
                         base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
            QCOMPARE(engine::ffmpegErrors() - errors, std::uint64_t{0});
            stream.push({keyFrameOf("desktop-720p15.h264", 1), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        }
    }

    // 2e, the second reading (M2): FFmpeg reads an SPS a second way where its first reading fails, as it came,
    // its emulation bytes left in. An SPS is made (scratchpad sps_raw_search.py, the commit's message) that
    // says 48 by 176 with them taken out and ends right after frame_mbs_only, which FFmpeg's first reading
    // refuses as read beyond its end; as it came, it is whole and says 1760 by 73200. Where an SPS has
    // emulation bytes it is read as it came too, and refused where that says a size the program does not
    // show, live or as a still: no memory is taken by that size. (Before: the gate read 48 by 176, and FFmpeg
    // took its tables by the other.) Among the first tests, for the process's most memory ever committed.
    void anSpsReadAsItCameReachesNoDecoder()
    {
        constexpr std::string_view twoWays =
            "100010110100000000000000000000000000010000000000000000110010010000110100101000000000000000001110000101"
            "001110110000000000000000000000000000001010101001101001010000000110111000000000000010001110111111000000"
            "101111000110101100010111";
        for (const engine::Codec codec : {engine::Codec::H264, engine::Codec::KeyFrameStill}) {
            const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
            QVERIFY(gpu != nullptr);
            const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
            engine::Stream& stream = scene->add(1, codec);
            stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
            const std::size_t before = mostEverCommitted();
            stream.push({keyFrameThatSays(1, 1, 0, 0, 66, twoWays), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
            const std::size_t after = mostEverCommitted();
            qInfo() << (codec == engine::Codec::H264 ? "live:" : "still:") << "most committed grew by"
                    << (after - before) / 1024 << "KB";
            QVERIFY2(after - before < std::size_t{16} * 1024 * 1024,
                     qPrintable(
                         QStringLiteral("%1 MB were taken for it").arg((after - before) / (std::size_t{1024} * 1024))));
            stream.push({keyFrameOf("desktop-720p15.h264", 1), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        }
    }

    // 2d and the SPS gate, after their reading (L2): parameter sets of two ids in one picture are lawful
    // (FFmpeg decodes them), and such a live stream shows; every SPS of a packet is read for its size,
    // not only the one its slice names, so a huge one of another id is refused; and two of one id that
    // differ are refused, also where both say a size that is shown.
    void aLiveStreamIsJudgedByEveryParameterSetById()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::H264);
        const std::vector<std::byte> real = keyFrameOf("desktop-720p15.h264", 0);
        QVERIFY2(!real.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        // A small SPS and its PPS of id 1 before the picture's own of id 0: shown.
        std::vector<std::byte> twoIds = headsOnly(keyFrameThatSays(20, 15, 1, 1));
        QVERIFY(!twoIds.empty());
        twoIds.insert(twoIds.end(), real.begin(), real.end());
        stream.push({twoIds, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
        // A huge SPS of id 1, the picture's own after it: refused.
        std::vector<std::byte> hugeOther = headsOnly(keyFrameThatSays(1000, 1000, 1, 1));
        const std::vector<std::byte> next = keyFrameOf("desktop-720p15.h264", 1);
        hugeOther.insert(hugeOther.end(), next.begin(), next.end());
        stream.push({hugeOther, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        // Two of id 0 that differ (the 1080p film's, then the picture's own): refused.
        std::vector<std::byte> sameIdTwice = headsOnly(keyFrameOf("desktop-1080p30.h264", 0));
        const std::vector<std::byte> third = keyFrameOf("desktop-720p15.h264", 2);
        sameIdTwice.insert(sameIdTwice.end(), third.begin(), third.end());
        stream.push({sameIdTwice, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{2});
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
    }

    // 2c, the second reading (L2): a key-frame still is judged by the one SPS it carries; one that carries
    // a second SPS, or PPS, other than its first, or a second picture, is refused, whatever the first
    // says. (The same SPS and PPS twice is what the WebRTC assembler hands over where the device sends
    // them in the picture too: that one is decoded.)
    void aKeyFrameStillOfMoreThanOnePictureIsRefused()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::KeyFrameStill);
        const std::vector<std::byte> one = keyFrameOf("desktop-720p15.h264", 0);
        QVERIFY2(!one.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        // The SPS and PPS twice, as the assembler hands them: decoded.
        std::vector<std::byte> twice = headsOnly(one);
        twice.insert(twice.end(), one.begin(), one.end());
        stream.push({twice, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        // Another SPS after its picture, of a huge one, and nothing more: refused.
        std::vector<std::byte> second = one;
        const std::vector<std::byte> huge = keyFrameThatSays(1000, 1000);
        const std::vector<Unit> hugeUnits = unitsOf(huge);
        QVERIFY(hugeUnits.size() >= 2 && hugeUnits[0].type == 7);
        second.insert(second.end(), huge.begin(), huge.begin() + static_cast<std::ptrdiff_t>(hugeUnits[1].start));
        // And another PPS alone: refused.
        std::vector<std::byte> third = one;
        third.insert(third.end(), huge.begin() + static_cast<std::ptrdiff_t>(hugeUnits[1].start),
                     huge.begin() + static_cast<std::ptrdiff_t>(hugeUnits[2].start));
        stream.push({second, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        stream.push({third, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{2});
        // Two pictures of one SPS: refused.
        std::vector<std::byte> two = keyFrameOf("desktop-720p15.h264", 1);
        const std::vector<std::byte> other = keyFrameOf("desktop-720p15.h264", 2);
        two.insert(two.end(), other.begin(), other.end());
        stream.push({two, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{3});
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
    }

    // A stream of plain stills (a kvmd's, a server's) takes no H.264, whatever its first bytes say: the
    // H.264 decoder is for the stills whose source says what they are.
    void aStillStreamTakesNoKeyFrame()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const std::vector<std::byte> key = keyFrameOf("desktop-720p15.h264", 0);
        QVERIFY2(!key.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        stream.push({key, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        QCOMPARE(stream.stats().decoded, std::uint64_t{0});
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
    }

    void aScreenshotHasTheRemoteResolutionAndColoursWithoutTheWall()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const auto scene = gpu->makeScene({});
        auto canvas = gpu->makeCanvas({800, 600});
        QVERIFY(canvas.ok());
        auto& stream = scene->add(7, engine::Codec::Still);
        QVERIFY(!scene->screenshot(7));
        const engine::Placed placed[] = {{7, {20, 30, 300, 180}}};
        scene->place(placed);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(!scene->screenshot(7));
        QVERIFY(scene->draw(canvas.value()->target()));
        const auto shot = scene->screenshot(7);
        QVERIFY(shot.ok());
        QCOMPARE(shot.value().size, (engine::SizePx{480, 270}));
        QCOMPARE(shot.value().pixels.size(), std::size_t{480} * 270 * 4);
        COMPARE_COLOUR(colourAt(shot.value().pixels, shot.value().size, 91, 97), red, 10);
        COMPARE_COLOUR(colourAt(shot.value().pixels, shot.value().size, 149, 97), green, 10);
        QCOMPARE(stream.stats().shown, std::uint64_t{0});
        scene->shown(base::steadyNowUs());
        QCOMPARE(stream.stats().shown, std::uint64_t{1});
        QVERIFY(!scene->draw(canvas.value()->target()));
        scene->place(placed);
        pushFile(stream, made("desktop-preview-480/002.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QVERIFY(scene->draw(canvas.value()->target()));
        const auto wall = canvas.value()->pixels();
        QVERIFY(wall.ok());
        COMPARE_COLOUR(colourAt(wall.value(), {800, 600}, 700, 500), black, 0);
        QVERIFY(!scene->screenshot(99));
        scene->remove(7);
        QVERIFY(!scene->screenshot(7));
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // The tests below end by asking Direct3D what it objected to. That means something only where
    // Direct3D checks at all, which needs a part of Windows that is not on every PC: said here, once,
    // so that "no objections" is never read as more than it is.
    void direct3dChecksTheEnginesCalls()
    {
        engine::Gpu::Options options;
        options.software = true;
        options.checked = true;
        if (!engine::Gpu::create(options)) {
            QSKIP("Windows' Graphics Tools are not installed: the engine's calls into Direct3D go unchecked in these "
                  "tests.");
        }
    }

    void aJpegIsDrawnWithItsColours()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        std::atomic<int> told{0};
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({[&] { ++told; }, {}, {}});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());

        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(told.load() >= 1);

        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), ground, 6);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 38), titleBar, 8);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 91, 97), red, 10);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 149, 97), green, 10);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 206, 97), amber, 10);

        const engine::Stats stats = stream.stats();
        QCOMPARE(stats.decoding, engine::Decoding::InSoftware);
        QCOMPARE(stats.picture, size);
        QCOMPARE(stats.shown, std::uint64_t{1});
        QCOMPARE(stats.refused, std::uint64_t{0});

        // Nothing new came, so nothing is drawn.
        QVERIFY(!scene->draw(canvas.value()->target()));
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void h264IsDrawnWithItsColours()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{640, 360};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());

        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        QCOMPARE(cuts.size(), std::size_t{150});
        QVERIFY(cuts.front().keyFrame);
        QVERIFY(!cuts[1].keyFrame);

        engine::Stream& stream = scene->add(7, engine::Codec::H264);
        const engine::Placed whole[] = {{7, {0, 0, size.width, size.height}}};
        scene->place(whole);

        // A picture that is not a key frame cannot begin a stream: it is counted and a key frame is wished for.
        const std::span<const std::byte> bytes(clip.value());
        stream.push({bytes.subspan(cuts[1].from, cuts[1].size), base::steadyNowUs(), false});
        QCOMPARE(stream.stats().droppedBehind, std::uint64_t{1});
        QVERIFY(stream.takeKeyFrameWish());
        QVERIFY(!stream.takeKeyFrameWish());

        // The key frame and the four pictures after it, one at a time as a KVM sends them.
        for (std::size_t index = 0; index < 5; ++index) {
            stream.push({bytes.subspan(cuts[index].from, cuts[index].size), base::steadyNowUs(), cuts[index].keyFrame});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{index + 1});
        }
        QVERIFY(scene->draw(canvas.value()->target()));
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 540, 270), ground, 8);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 200, 190), panel, 8);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 200, 50), titleBar, 10);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 121, 129), red, 12);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 198, 129), green, 12);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 275, 129), amber, 12);

        const engine::Stats stats = stream.stats();
        QCOMPARE(stats.decoding, engine::Decoding::InSoftware);
        QCOMPARE(stats.picture, (engine::SizePx{1280, 720}));
        QCOMPARE(stats.replaced, std::uint64_t{4}); // five decoded, one drawn: four made way for a newer one
        QCOMPARE(stats.refused, std::uint64_t{0});
        const auto shot = scene->screenshot(7);
        QVERIFY(shot.ok());
        QCOMPARE(shot.value().size, (engine::SizePx{1280, 720}));
        COMPARE_COLOUR(colourAt(shot.value().pixels, shot.value().size, 242, 258), red, 12);
        COMPARE_COLOUR(colourAt(shot.value().pixels, shot.value().size, 396, 258), green, 12);
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // Where the time from arriving to the screen goes: the decoded picture waited 150 ms for the
    // draw, and the frame took 50 ms more to be shown; waiting for the decoder and decoding took
    // what they took, which is far less here.
    // A new picture of a stream shown large is told to the host as one to draw as soon as can be; a
    // wall tile's as one to draw at the host's own pace; and a host that has no way for the first
    // is told the second.
    void aStreamShownLargeAsksToBeDrawnAtOnce()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        std::atomic<int> soon{0};
        std::atomic<int> atOnce{0};
        const std::unique_ptr<engine::Scene> scene =
            gpu->makeScene({[&] { ++soon; }, [](engine::Fault) {}, [&] { ++atOnce; }});
        engine::Stream& large = scene->add(16, engine::Codec::Still);
        engine::Stream& tile = scene->add(17, engine::Codec::Still);
        large.shownAt({1280, 720}, engine::Importance::ShownLarge);
        // (The picture is counted as decoded a moment before the host is told of it.)
        pushFile(large, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(atOnce.load(), 1);
        QCOMPARE(soon.load(), 0);
        pushFile(tile, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(soon.load(), 1);
        QCOMPARE(atOnce.load(), 1);

        std::atomic<int> told{0};
        const std::unique_ptr<engine::Scene> plain = gpu->makeScene({[&] { ++told; }, {}, {}});
        engine::Stream& another = plain->add(18, engine::Codec::Still);
        another.shownAt({1280, 720}, engine::Importance::Controlled);
        pushFile(another, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(told.load(), 1);
    }

    void whereTheTimeFromArrivingToTheScreenGoesIsCounted()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{320, 180};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        const std::span<const std::byte> bytes(clip.value());
        engine::Stream& stream = scene->add(15, engine::Codec::H264);
        const engine::Placed whole[] = {{15, {0, 0, size.width, size.height}}};
        scene->place(whole);

        const std::int64_t arrived = base::steadyNowUs();
        stream.push({bytes.subspan(cuts[0].from, cuts[0].size), arrived, true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QTest::qWait(150);
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs() + 50'000);

        const engine::Stats stats = stream.stats();
        QVERIFY2(stats.readyToDrawnMs >= 150, qPrintable(QString::number(stats.readyToDrawnMs)));
        QVERIFY2(stats.drawnToShownMs >= 50, qPrintable(QString::number(stats.drawnToShownMs)));
        QVERIFY2(stats.waitedMs >= 0 && stats.waitedMs < 150, qPrintable(QString::number(stats.waitedMs)));
        // The four together are the whole from arriving to the screen.
        const double parts = stats.waitedMs + stats.decodeMs + stats.readyToDrawnMs + stats.drawnToShownMs;
        QVERIFY2(
            std::abs(parts - stats.arrivedToShownMs) < 1.0,
            qPrintable(QString::number(parts) + QStringLiteral(" against ") + QString::number(stats.arrivedToShownMs)));
        // Up to each of arrivedToShownStepsMs: over 100, and between 33 and 100 (or over, where the
        // drawing itself took long on this PC).
        QCOMPARE(stats.readyToDrawnCounts[7], std::uint64_t{1});
        QCOMPARE(stats.drawnToShownCounts[6] + stats.drawnToShownCounts[7], std::uint64_t{1});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // Smooth is even steps: the time between two new pictures is counted as their packets arrived and
    // as they reached the screen, so that unevenness on the screen that the arrivals do not have is
    // known to be this program's own.
    void theStepsBetweenPicturesAreCounted()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{320, 180};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());

        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        QVERIFY(cuts.size() >= 8);
        const std::span<const std::byte> bytes(clip.value());

        engine::Stream& stream = scene->add(3, engine::Codec::H264);
        const engine::Placed whole[] = {{3, {0, 0, size.width, size.height}}};
        scene->place(whole);

        // When each packet arrived and when its picture was shown, in microseconds from the first:
        // the arrivals 16.7, 40, 300 and 5 ms apart; on the screen 17, 60 and 120 ms apart, the
        // fourth picture made way for the fifth before it was drawn. From arriving to the screen
        // each shown picture then took 0.3, 20 and -185 ms longer than the one before it.
        const std::int64_t arrived = base::steadyNowUs() - 1'000'000;
        const std::int64_t onScreen = base::steadyNowUs();
        const std::int64_t arrivals[] = {0, 16'667, 56'667, 356'667, 361'667};
        const std::int64_t shownAt[] = {0, 17'000, 77'000, -1, 197'000};
        for (std::size_t index = 0; index < 5; ++index) {
            stream.push(
                {bytes.subspan(cuts[index].from, cuts[index].size), arrived + arrivals[index], cuts[index].keyFrame});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{index + 1});
            if (shownAt[index] < 0) {
                continue;
            }
            QVERIFY(scene->draw(canvas.value()->target()));
            scene->shown(onScreen + shownAt[index]);
        }

        // Up to each of gapStepsMs, and in the last place everything slower; the first picture
        // has no step before it.
        const engine::Stats stats = stream.stats();
        QCOMPARE(stats.shown, std::uint64_t{4});
        QCOMPARE(stats.replaced, std::uint64_t{1});
        const std::array<std::uint64_t, 8> arrivalSteps{1, 1, 0, 1, 0, 0, 0, 1};
        const std::array<std::uint64_t, 8> shownSteps{0, 1, 0, 0, 1, 0, 1, 0};
        QCOMPARE(stats.arrivalGapCounts, arrivalSteps);
        QCOMPARE(stats.shownGapCounts, shownSteps);
        QCOMPARE(stats.shownGapMaxMs, 120.0);
        const std::array<std::uint64_t, 8> uneven{1, 0, 0, 0, 0, 1, 0, 1};
        QCOMPARE(stats.unevenCounts, uneven);

        // A packet that does not say when it arrived is no step, and the step after it is from the
        // packet before it; its picture is a step on the screen, but no change of the delay, and
        // the picture after it changes none either: there is nothing to change from.
        stream.push({bytes.subspan(cuts[5].from, cuts[5].size), 0, cuts[5].keyFrame});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{6});
        QCOMPARE(stream.stats().arrivalGapCounts, arrivalSteps);
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(onScreen + 213'000); // 16 ms after the one before: up to 21
        // A step of exactly 21 ms is counted as up to 21, and one of exactly 12 as up to 12.
        stream.push({bytes.subspan(cuts[6].from, cuts[6].size), arrived + 361'667 + 21'000, cuts[6].keyFrame});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{7});
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(onScreen + 225'000);
        const engine::Stats after = stream.stats();
        QCOMPARE(after.arrivalGapCounts, (std::array<std::uint64_t, 8>{1, 2, 0, 1, 0, 0, 0, 1}));
        QCOMPARE(after.shownGapCounts, (std::array<std::uint64_t, 8>{1, 2, 0, 0, 1, 0, 1, 0}));
        QCOMPARE(after.unevenCounts, uneven);

        // A frame it is not drawn in: the next step on the screen is not counted from before it.
        scene->place({});
        static_cast<void>(scene->draw(canvas.value()->target()));
        scene->shown(onScreen + 300'000);
        scene->place(whole);
        stream.push({bytes.subspan(cuts[7].from, cuts[7].size), arrived + 382'667 + 16'667, cuts[7].keyFrame});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{8});
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(onScreen + 900'000);
        QCOMPARE(stream.stats().shownGapCounts, after.shownGapCounts);
        QCOMPARE(stream.stats().unevenCounts, uneven);
        QCOMPARE(stream.stats().shown, std::uint64_t{7});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // How H.264 is decoded is chosen while the program runs: a stream whose way of decoding that
    // changes wishes for a key frame to change at, so that it need not wait for the KVM to send
    // one by itself; one whose way stays the same, or cannot change, asks for nothing. Windows'
    // software adapter decodes nothing itself, so here nothing changes: what changes on a card is
    // the test after this one.
    void theWayOfDecodingIsChangedWhileStreamsRun()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        QCOMPARE(gpu->decoding(), engine::DecodingChoice::Automatic);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        QVERIFY(cuts.size() >= 4);
        const std::span<const std::byte> bytes(clip.value());
        engine::Stream& stream = scene->add(5, engine::Codec::H264);
        engine::Stream& still = scene->add(6, engine::Codec::Still);
        const auto push = [&](std::size_t index) {
            stream.push({bytes.subspan(cuts[index].from, cuts[index].size), base::steadyNowUs(), cuts[index].keyFrame});
        };
        push(0);
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(!stream.takeKeyFrameWish());

        // In software already: nothing changes, and nothing is asked for.
        gpu->setDecoding(engine::DecodingChoice::Software);
        QCOMPARE(gpu->decoding(), engine::DecodingChoice::Software);
        push(1);
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QVERIFY(!stream.takeKeyFrameWish());
        // On the card, which this device has not: no change can be made, and none is asked for.
        QVERIFY(!gpu->decodesH264());
        gpu->setDecoding(engine::DecodingChoice::GraphicsCard);
        push(2);
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{3});
        QVERIFY(!stream.takeKeyFrameWish());
        push(3);
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{4});
        QVERIFY(!stream.takeKeyFrameWish());
        // A JPEG stands by itself and wishes for nothing.
        pushFile(still, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(still.stats().decoded, std::uint64_t{1});
        QVERIFY(!still.takeKeyFrameWish());
        // The same choice again.
        gpu->setDecoding(engine::DecodingChoice::GraphicsCard);
        push(4);
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{5});
        QVERIFY(!stream.takeKeyFrameWish());
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
    }

    // On a graphics card that decodes H.264: a stream shown large is decoded on it; chosen off, it
    // asks for a key frame and is decoded in software from there; chosen automatic again, on the
    // card again from its next key frame; and chosen off just before a key frame comes, it changes
    // at that one and asks for none.
    void aStreamChangesBetweenTheCardAndSoftwareAtItsNextKeyFrame()
    {
        base::Result<std::unique_ptr<engine::Gpu>> card = engine::Gpu::create({});
        if (!card || !card.value()->decodesH264()) {
            QSKIP("This PC has no graphics card that decodes H.264.");
        }
        engine::Gpu& gpu = *card.value();
        const std::unique_ptr<engine::Scene> scene = gpu.makeScene({});
        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        // (A key frame every two seconds: at 0, 30, 60 and 90, and at none between.)
        QVERIFY(cuts.size() >= 92);
        for (std::size_t index = 0; index <= 91; ++index) {
            QCOMPARE(cuts[index].keyFrame, index % 30 == 0);
        }
        const std::span<const std::byte> bytes(clip.value());
        engine::Stream& stream = scene->add(9, engine::Codec::H264);
        stream.shownAt({1280, 720}, engine::Importance::ShownLarge);
        std::uint64_t decoded = 0;
        const auto push = [&](std::size_t index) {
            stream.push({bytes.subspan(cuts[index].from, cuts[index].size), base::steadyNowUs(), cuts[index].keyFrame});
            ++decoded;
            return QTest::qWaitFor([&] { return stream.stats().decoded >= decoded; }, 5000);
        };
        for (std::size_t index = 0; index < 5; ++index) {
            QVERIFY(push(index));
        }
        if (stream.stats().decoding != engine::Decoding::OnTheCard) {
            QSKIP("This PC's graphics card says it decodes H.264, but would not take this film.");
        }
        QVERIFY(!stream.takeKeyFrameWish());

        gpu.setDecoding(engine::DecodingChoice::Software);
        QVERIFY(push(5));
        QVERIFY(stream.takeKeyFrameWish());
        // Until the key frame the card goes on; from it, software.
        for (std::size_t index = 6; index < 30; ++index) {
            QVERIFY(push(index));
        }
        QCOMPARE(stream.stats().decoding, engine::Decoding::OnTheCard);
        QVERIFY(push(30));
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);

        gpu.setDecoding(engine::DecodingChoice::Automatic);
        QVERIFY(push(31));
        QVERIFY(stream.takeKeyFrameWish());
        for (std::size_t index = 32; index <= 60; ++index) {
            QVERIFY(push(index));
        }
        QCOMPARE(stream.stats().decoding, engine::Decoding::OnTheCard);
        QVERIFY(!stream.takeKeyFrameWish());

        for (std::size_t index = 61; index < 90; ++index) {
            QVERIFY(push(index));
        }
        gpu.setDecoding(engine::DecodingChoice::Software);
        QVERIFY(push(90));
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
        QVERIFY(push(91));
        QVERIFY(!stream.takeKeyFrameWish());
    }

    // The device is lost while a stream's key frame comes: it is decoded in software, as there is
    // no card then. When the device is back, the stream asks for a key frame to go back to the
    // card at, and is on it from there; a KVM that sends a key frame only when asked would leave it
    // in software otherwise.
    void aStreamGoesBackToTheCardAfterTheDeviceCameBack()
    {
        base::Result<std::unique_ptr<engine::Gpu>> card = engine::Gpu::create({});
        if (!card || !card.value()->decodesH264()) {
            QSKIP("This PC has no graphics card that decodes H.264.");
        }
        engine::Gpu& gpu = *card.value();
        const std::unique_ptr<engine::Scene> scene = gpu.makeScene({});
        const engine::SizePx size{320, 180};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu.makeCanvas(size);
        QVERIFY(canvas.ok());
        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        QVERIFY(cuts.size() >= 62);
        const std::span<const std::byte> bytes(clip.value());
        engine::Stream& stream = scene->add(11, engine::Codec::H264);
        stream.shownAt({1280, 720}, engine::Importance::ShownLarge);
        const engine::Placed whole[] = {{11, {0, 0, size.width, size.height}}};
        scene->place(whole);
        std::uint64_t handedIn = 0;
        const auto push = [&](std::size_t index) {
            stream.push({bytes.subspan(cuts[index].from, cuts[index].size), base::steadyNowUs(), cuts[index].keyFrame});
            ++handedIn;
            return QTest::qWaitFor([&] { return stream.stats().decoded + stream.stats().droppedBehind >= handedIn; },
                                   5000);
        };
        for (std::size_t index = 0; index < 5; ++index) {
            QVERIFY(push(index));
        }
        if (stream.stats().decoding != engine::Decoding::OnTheCard) {
            QSKIP("This PC's graphics card says it decodes H.264, but would not take this film.");
        }

        // Lost, and nothing has drawn since: the key frame at 30 comes while it is gone.
        gpu.loseDevice();
        for (std::size_t index = 5; index <= 30; ++index) {
            QVERIFY(push(index));
        }
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
        static_cast<void>(stream.takeKeyFrameWish());

        // A draw finds it lost and makes it anew.
        static_cast<void>(scene->draw(canvas.value()->target()));
        QCOMPARE(gpu.timesRecovered(), std::uint64_t{1});
        QVERIFY(push(31));
        QVERIFY(stream.takeKeyFrameWish());
        for (std::size_t index = 32; index <= 60; ++index) {
            QVERIFY(push(index));
        }
        QCOMPARE(stream.stats().decoding, engine::Decoding::OnTheCard);
        QVERIFY(!stream.takeKeyFrameWish());
    }

    // 2c, the second reading (M): a machine shown large and decoded on the card whose decoder gives a new
    // texture (its picture's size changed), and the views of it cannot be made, with the device as it
    // was (the card's memory full): the views of the texture before went to make room, and nothing is
    // drawn from them; the tile is empty, and the next picture is drawn. (Before: what was drawn still
    // named those views, and every frame drew from them.) Where this PC's card decodes H.264.
    void aCardPictureWhoseViewsCannotBeMadeLeavesNothingDrawnThatWent()
    {
        base::Result<std::unique_ptr<engine::Gpu>> card = engine::Gpu::create({});
        if (!card || !card.value()->decodesH264()) {
            QSKIP("This PC has no graphics card that decodes H.264.");
        }
        engine::Gpu& gpu = *card.value();
        const std::unique_ptr<engine::Scene> scene = gpu.makeScene({});
        const engine::SizePx size{640, 360};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu.makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(11, engine::Codec::H264);
        stream.shownAt({1280, 720}, engine::Importance::ShownLarge);
        const engine::Placed whole[] = {{11, {0, 0, size.width, size.height}}};
        scene->place(whole);
        stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        if (stream.stats().decoding != engine::Decoding::OnTheCard) {
            QSKIP("This PC's graphics card says it decodes H.264, but would not take this film.");
        }
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        // Another size: the card's decoder makes a texture anew.
        stream.push({keyFrameOf("desktop-1080p30.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        gpu.failTextures(1);
        QVERIFY(scene->draw(canvas.value()->target()));
        QCOMPARE(stream.stats().refused, std::uint64_t{1});
        base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 200, 190), black, 0);
        scene->shown(base::steadyNowUs());
        stream.push({keyFrameOf("desktop-1080p30.h264", 1), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{3});
        QVERIFY(scene->draw(canvas.value()->target()));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 200, 190), panel, 8);
    }

    // 2d and the SPS gate, after their reading (M): a key frame the gate refuses (its SPS says too large a
    // picture: the packet's fault, not the card's) leaves the machine shown large on the card: the next
    // good key frame is decoded there. (Before: the stream gave up on the card for the session.) Where
    // this PC's card decodes H.264.
    void aKeyFrameTheGateRefusesLeavesTheStreamOnTheCard()
    {
        base::Result<std::unique_ptr<engine::Gpu>> card = engine::Gpu::create({});
        if (!card || !card.value()->decodesH264()) {
            QSKIP("This PC has no graphics card that decodes H.264.");
        }
        const std::unique_ptr<engine::Scene> scene = card.value()->makeScene({});
        engine::Stream& stream = scene->add(11, engine::Codec::H264);
        stream.shownAt({1280, 720}, engine::Importance::ShownLarge);
        stream.push({keyFrameOf("desktop-720p15.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        if (stream.stats().decoding != engine::Decoding::OnTheCard) {
            QSKIP("This PC's graphics card says it decodes H.264, but would not take this film.");
        }
        stream.push({keyFrameThatSays(1000, 1000), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        stream.push({keyFrameOf("desktop-720p15.h264", 1), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QCOMPARE(stream.stats().decoding, engine::Decoding::OnTheCard);
    }

    // ... and a stream that goes from the card to software (Settings chose Off) lets go of the card
    // decoder's texture once its first picture in software is drawn (before, the views of it kept it for
    // the session). Measured, not held: the card's memory the process holds, as Windows counts it, did not
    // move on this PC's driver either way (89 MB before and after), so what is let go is held by reading.
    void aStreamGoneToSoftwareLetsGoOfTheCardsTexture()
    {
        base::Result<std::unique_ptr<engine::Gpu>> card = engine::Gpu::create({});
        if (!card || !card.value()->decodesH264()) {
            QSKIP("This PC has no graphics card that decodes H.264.");
        }
        engine::Gpu& gpu = *card.value();
        const std::unique_ptr<engine::Scene> scene = gpu.makeScene({});
        const engine::SizePx size{640, 360};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu.makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(11, engine::Codec::H264);
        stream.shownAt({1920, 1080}, engine::Importance::ShownLarge);
        const engine::Placed whole[] = {{11, {0, 0, size.width, size.height}}};
        scene->place(whole);
        stream.push({keyFrameOf("pattern-1080p30.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        if (stream.stats().decoding != engine::Decoding::OnTheCard) {
            QSKIP("This PC's graphics card says it decodes H.264, but would not take this film.");
        }
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        const std::uint64_t onTheCard = gpu.memory().usedBytes;
        gpu.setDecoding(engine::DecodingChoice::Software);
        stream.push({keyFrameOf("pattern-1080p30.h264", 1), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        // (Direct3D lets go of what nothing holds as later frames are drawn.)
        for (std::size_t next = 2; next < 5; ++next) {
            stream.push({keyFrameOf("pattern-1080p30.h264", next), base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, std::uint64_t{next + 1});
            QVERIFY(scene->draw(canvas.value()->target()));
            scene->shown(base::steadyNowUs());
        }
        QTest::qWait(300);
        const std::uint64_t inSoftware = gpu.memory().usedBytes;
        qInfo() << "the card's memory on the card:" << onTheCard / 1'000'000
                << "MB, in software:" << inSoftware / 1'000'000 << "MB";
    }

    // Which adapter a choice comes to on this PC, and the device made for that choice is on it;
    // on a PC with Windows' software adapter alone, each comes to that one.
    void eachChoiceOfAdapterIsTheAdapterTheDeviceIsMadeOn_data()
    {
        QTest::addColumn<engine::AdapterChoice>("choice");
        QTest::newRow("the most powerful") << engine::AdapterChoice::MostPowerful;
        QTest::newRow("the one that saves power") << engine::AdapterChoice::PowerSaving;
        QTest::newRow("as Windows chooses") << engine::AdapterChoice::AsWindowsChooses;
    }

    void eachChoiceOfAdapterIsTheAdapterTheDeviceIsMadeOn()
    {
        QFETCH(const engine::AdapterChoice, choice);
        if (engine::Gpu::adapters().empty()) {
            QSKIP("Windows lists no graphics adapter on this PC.");
        }
        const std::string name = engine::Gpu::adapterFor(choice);
        QVERIFY(!name.empty());
        engine::Gpu::Options options;
        options.choice = choice;
        base::Result<std::unique_ptr<engine::Gpu>> made = engine::Gpu::create(options);
        if (!made && !engine::Gpu::create({})) {
            QSKIP("No Direct3D device can be made on this PC's graphics adapters.");
        }
        QVERIFY(made.ok());
        QCOMPARE(made.value()->adapterName(), name);
    }

    // The most powerful and the one that saves power are as Windows orders the adapters for each:
    // what it gives first, asked by power and asked by saving.
    void theChoicesAreWindowsOwnOrderOfTheAdapters()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            QSKIP("This Windows does not order its adapters by power.");
        }
        const auto firstBy = [&factory](DXGI_GPU_PREFERENCE preference) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, preference, IID_PPV_ARGS(&adapter)))) {
                adapter->GetDesc1(&description);
            }
            const QString name = QString::fromWCharArray(description.Description);
            return name.toStdString();
        };
        QCOMPARE(engine::Gpu::adapterFor(engine::AdapterChoice::MostPowerful),
                 firstBy(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE));
        QCOMPARE(engine::Gpu::adapterFor(engine::AdapterChoice::PowerSaving),
                 firstBy(DXGI_GPU_PREFERENCE_MINIMUM_POWER));
    }

    // Whether a monitor is on the adapter a choice comes to: as that adapter's own list of outputs
    // says. (On this PC's kind, a card with the monitor and the processor's graphics without one, the
    // two choices differ; elsewhere they may not.)
    void whetherAMonitorIsOnTheAdapterIsAsItsOutputsSay_data()
    {
        QTest::addColumn<engine::AdapterChoice>("choice");
        QTest::addColumn<int>("preference");
        QTest::newRow("the most powerful")
            << engine::AdapterChoice::MostPowerful << static_cast<int>(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE);
        QTest::newRow("the one that saves power")
            << engine::AdapterChoice::PowerSaving << static_cast<int>(DXGI_GPU_PREFERENCE_MINIMUM_POWER);
    }

    void whetherAMonitorIsOnTheAdapterIsAsItsOutputsSay()
    {
        QFETCH(const engine::AdapterChoice, choice);
        QFETCH(const int, preference);
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByGpuPreference(0, static_cast<DXGI_GPU_PREFERENCE>(preference),
                                                       IID_PPV_ARGS(&adapter)))) {
            QSKIP("This Windows does not order its adapters by power.");
        }
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        QCOMPARE(engine::Gpu::adapterHasMonitor(choice), adapter->EnumOutputs(0, &output) == S_OK);
    }

    void aPictureKeepsItsShapeInItsTile()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{400, 400};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());

        // A wide picture in a square tile that takes the right half of the target's upper part:
        // the picture lies across the tile's middle, and above and below it the tile is black.
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed placed[] = {{1, {200, 0, 200, 200}}};
        scene->place(placed);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());

        // 480 by 270 in 200 by 200 is 200 by 113 (112.5 rounded), from row 43 on.
        COMPARE_COLOUR(colourAt(pixels.value(), size, 300, 20), black, 0);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 300, 180), black, 0);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 262, 101), panel, 8);  // the picture's 150,140
        COMPARE_COLOUR(colourAt(pixels.value(), size, 366, 126), ground, 8); // the picture's 400,200
        // Beside the tile nothing of it shows.
        COMPARE_COLOUR(colourAt(pixels.value(), size, 100, 100), black, 0);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 300, 300), black, 0);
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // b10: a picture larger than the view it is shown in (the enlarged view at a scale) is drawn only
    // inside that view, the rest of its place being left as it was; and one placed without such a view
    // is drawn whole, as ever.
    void aPictureIsDrawnOnlyInsideTheViewItIsShownIn()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});

        // Shown in the left half of the target: of the picture only that half.
        const engine::Placed left[] = {{1, {0, 0, 480, 270}, {0, 0, 240, 270}}};
        scene->place(left);
        QVERIFY(scene->draw(canvas.value()->target()));
        base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), black, 0);
        // In the right half, and a view that reaches beyond the target: that half.
        const engine::Placed right[] = {{1, {0, 0, 480, 270}, {240, -20, 400, 400}}};
        scene->place(right);
        QVERIFY(scene->draw(canvas.value()->target()));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), ground, 6);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), black, 0);
        // A view beside the target: nothing of it.
        const engine::Placed beside[] = {{1, {0, 0, 480, 270}, {500, 0, 100, 100}}};
        scene->place(beside);
        QVERIFY(scene->draw(canvas.value()->target()));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), black, 0);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), black, 0);
        // None: whole.
        const engine::Placed whole[] = {{1, {0, 0, 480, 270}}};
        scene->place(whole);
        QVERIFY(scene->draw(canvas.value()->target()));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), ground, 6);
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void aLostDeviceComesBackByItself()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        std::vector<engine::Fault> faults;
        const std::unique_ptr<engine::Scene> scene =
            gpu->makeScene({{}, [&](engine::Fault fault) { faults.push_back(fault); }, {}});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        const std::int64_t at = base::steadyNowUs();
        scene->shown(at);

        gpu->loseDevice();
        QVERIFY(!scene->screenshot(1));
        // The draw that finds the device gone makes another, and draws nothing: what it was given to
        // draw into went with the old device. The picture drawn went with it too, and the stream
        // decodes the last one it had again (a paused wall gets no next one): it is there again
        // without a new one coming. (Until 2026-10-27 the tile was empty until the next picture.)
        QVERIFY(!scene->draw(canvas.value()->target()));
        QCOMPARE(gpu->timesRecovered(), std::uint64_t{1});
        QCOMPARE(faults, (std::vector<engine::Fault>{engine::Fault::DeviceLost, engine::Fault::DeviceRestored}));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QVERIFY(scene->draw(canvas.value()->target()));
        base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        // It is that one picture again, and nothing more: the stream's count of what came is as it was.
        QCOMPARE(stream.stats().pushed, std::uint64_t{1});
        // And lost a second time, before anything new came: it is there again, all the same.
        gpu->loseDevice();
        QVERIFY(!scene->draw(canvas.value()->target()));
        QCOMPARE(gpu->timesRecovered(), std::uint64_t{2});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{3});
        QVERIFY(scene->draw(canvas.value()->target()));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);

        pushFile(stream, made("desktop-preview-480/002.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{4});
        QVERIFY(scene->draw(canvas.value()->target()));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        // On the screen again: no step is counted across the time the device was gone.
        scene->shown(at + 5'000'000);
        QCOMPARE(stream.stats().shown, std::uint64_t{2});
        QCOMPARE(stream.stats().shownGapCounts, (std::array<std::uint64_t, 8>{}));
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // The device is lost and made anew: the picture a stream had drawn went with it. An H.264
    // stream decoded in software asks for a key frame to draw again from, as one on the card does;
    // a JPEG stands by itself and asks for nothing.
    void aStreamInSoftwareAsksForAKeyFrameWhenTheDeviceIsBack()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{320, 180};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        const std::span<const std::byte> bytes(clip.value());
        engine::Stream& stream = scene->add(12, engine::Codec::H264);
        engine::Stream& still = scene->add(13, engine::Codec::Still);
        const engine::Placed both[] = {{12, {0, 0, 160, 180}}, {13, {160, 0, 160, 180}}};
        scene->place(both);
        stream.push({bytes.subspan(cuts[0].from, cuts[0].size), base::steadyNowUs(), true});
        pushFile(still, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QTRY_COMPARE(still.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
        QVERIFY(!stream.takeKeyFrameWish());

        gpu->loseDevice();
        static_cast<void>(scene->draw(canvas.value()->target()));
        QCOMPARE(gpu->timesRecovered(), std::uint64_t{1});
        QVERIFY(stream.takeKeyFrameWish());
        QVERIFY(!still.takeKeyFrameWish());
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // Packets handed in the moment the picture before them is out, again and again, by several
    // streams at once: every one is decoded, and none is left lying until another comes. (What
    // this can catch it catches by chance: a packet that came while a decoding thread was letting
    // go of its stream. tools/test_review_guards.ps1 holds the order that rules it out.)
    void aPacketThatComesAsTheOneBeforeItIsDoneIsDecodedAllTheSame()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const base::Result<std::vector<std::byte>> clip = bench::readFile(made("desktop-720p15.h264"));
        QVERIFY2(clip.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<bench::Cut> cuts = bench::cutIntoPictures(clip.value());
        const std::span<const std::byte> bytes(clip.value());
        constexpr int streams = 4;
        std::vector<engine::Stream*> all;
        all.reserve(streams);
        for (int number = 0; number < streams; ++number) {
            all.push_back(&scene->add(static_cast<engine::StreamId>(20 + number), engine::Codec::H264));
        }
        std::atomic<int> late{0};
        std::vector<std::thread> pushers;
        pushers.reserve(all.size());
        for (engine::Stream* stream : all) {
            pushers.emplace_back([stream, &cuts, bytes, &late] {
                for (std::size_t index = 0; index < cuts.size(); ++index) {
                    stream->push(
                        {bytes.subspan(cuts[index].from, cuts[index].size), base::steadyNowUs(), cuts[index].keyFrame});
                    // The next one goes in the moment this one is out.
                    const std::int64_t until = base::steadyNowUs() + 3'000'000;
                    while (stream->stats().decoded < index + 1) {
                        if (base::steadyNowUs() > until) {
                            late.fetch_add(1);
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::thread& pusher : pushers) {
            pusher.join();
        }
        QCOMPARE(late.load(), 0);
        for (const engine::Stream* stream : all) {
            QCOMPARE(stream->stats().decoded, std::uint64_t{cuts.size()});
        }
    }

    // A newer picture than the one drawn waits to be drawn as the device is lost: it is the one
    // drawn when the device is back, and the older one is not decoded again over it. And asking
    // for the last picture again is no packet that was handed in, and none that was thrown away.
    void aNewerPictureThatWaitsIsNotTakenOverByTheOlderAfterALoss()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(14, engine::Codec::Still);
        const engine::Placed whole[] = {{14, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        pushFile(stream, made("desktop-preview-480/002.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});

        gpu->loseDevice();
        QVERIFY(!scene->draw(canvas.value()->target()));
        QCOMPARE(gpu->timesRecovered(), std::uint64_t{1});
        QTest::qWait(300);
        const engine::Stats stats = stream.stats();
        QCOMPARE(stats.decoded, std::uint64_t{2});
        QCOMPARE(stats.replaced, std::uint64_t{0});
        QCOMPARE(stats.pushed, std::uint64_t{2});
        QCOMPARE(stats.droppedBehind, std::uint64_t{0});
        QVERIFY(scene->draw(canvas.value()->target()));
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void aPngIsDrawnWithItsColours_data()
    {
        QTest::addColumn<QByteArray>("file");
        QTest::addColumn<int>("holds");
        QTest::addColumn<int>("within");
        // The desktop once in each way a PNG can hold a picture. A palette made from the picture and
        // a picture with sixteen bits a value come out as the plain one does.
        QTest::newRow("red, green and blue") << QByteArray("rgb") << int{Colours} << 4;
        QTest::newRow("sixteen bits a value") << QByteArray("rgb-16") << int{Colours} << 4;
        QTest::newRow("a palette") << QByteArray("palette") << int{Colours} << 4;
        QTest::newRow("seen through by half, over black") << QByteArray("rgba-half") << int{HalfTheColours} << 3;
        QTest::newRow("greys") << QByteArray("grey") << int{Greys} << 3;
        QTest::newRow("greys of sixteen bits") << QByteArray("grey-16") << int{Greys} << 3;
        QTest::newRow("greys with nothing seen through") << QByteArray("grey-alpha") << int{Greys} << 3;
        QTest::newRow("black and white") << QByteArray("black-white") << int{BlackAndWhite} << 0;
    }

    void aPngIsDrawnWithItsColours()
    {
        QFETCH(QByteArray, file);
        QFETCH(int, holds);
        QFETCH(int, within);
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made(("desktop-stills-png/" + file + ".png").constData()));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
        QCOMPARE(stream.stats().picture, size);

        QVERIFY(scene->draw(canvas.value()->target()));
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        if (holds == BlackAndWhite) {
            // Which pixel is which is the business of whoever made the picture. Here: nothing but
            // black and white, and both of them.
            int blacks = 0;
            int whites = 0;
            for (int y = 0; y < size.height; ++y) {
                for (int x = 0; x < size.width; ++x) {
                    const Colour is = colourAt(pixels.value(), size, x, y);
                    blacks += closeTo(is, black, 0) ? 1 : 0;
                    whites += closeTo(is, Colour{255, 255, 255}, 0) ? 1 : 0;
                }
            }
            QCOMPARE(blacks + whites, size.width * size.height);
            QVERIFY(blacks > 1000 && whites > 1000);
        } else {
            const auto wanted = [holds](Colour colour) {
                if (holds == HalfTheColours) {
                    return Colour{colour.red * 128 / 255, colour.green * 128 / 255, colour.blue * 128 / 255};
                }
                if (holds == Greys) {
                    // How light a colour is, as the small-screen standard weighs red, green and blue.
                    const int light = (299 * colour.red + 587 * colour.green + 114 * colour.blue + 500) / 1000;
                    return Colour{light, light, light};
                }
                return colour;
            };
            COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), wanted(ground), within);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), wanted(panel), within);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 38), wanted(titleBar), within);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 91, 97), wanted(red), within);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 149, 97), wanted(green), within);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 206, 97), wanted(amber), within);
        }
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void oneStreamTakesJpegsAndPngsInTurn()
    {
        // As a KVM does that sends a PNG while it has no signal, and JPEGs otherwise.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        const Colour halfThePanel{panel.red * 128 / 255, panel.green * 128 / 255, panel.blue * 128 / 255};

        const struct {
            const char* file;
            Colour panelIs;
        } inTurn[] = {
            {"desktop-preview-480/001.jpg", panel}, {"desktop-stills-png/rgba-half.png", halfThePanel},
            {"desktop-preview-480/002.jpg", panel}, {"desktop-stills-png/grey.png", Colour{40, 40, 40}},
            {"desktop-stills-png/rgb.png", panel},
        };
        std::uint64_t decoded = 0;
        for (const auto& one : inTurn) {
            pushFile(stream, made(one.file));
            ++decoded;
            QTRY_COMPARE(stream.stats().decoded, decoded);
            QVERIFY(scene->draw(canvas.value()->target()));
            const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
            QVERIFY(pixels.ok());
            COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), one.panelIs, 6);
        }
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void aPngAfterAPngOfTheSameKindIsDrawnAsItself()
    {
        // As a KVM does that sends one PNG after the other, all of one size and kind. FFmpeg's
        // decoder of PNGs takes a packet that is not marked as a whole picture for what has changed
        // since the last one, and adds the two together. (Each with a note of its own since M6 2c: a
        // still byte for byte the last one decoded is let go, and these are the same picture.)
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);

        for (std::uint64_t time = 1; time <= 3; ++time) {
            const std::vector<std::byte> png = pngWithANote(made("desktop-stills-png/rgb.png"), static_cast<int>(time));
            QVERIFY2(!png.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
            stream.push({png, base::steadyNowUs(), true});
            QTRY_COMPARE(stream.stats().decoded, time);
            QVERIFY(scene->draw(canvas.value()->target()));
            const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
            QVERIFY(pixels.ok());
            COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), ground, 3);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 3);
            COMPARE_COLOUR(colourAt(pixels.value(), size, 91, 97), red, 3);
        }
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{0});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // M6 2c (design 4, D-e): a card's still may be an H.264 key frame, as a JetKVM's connection holds
    // it: SPS and PPS in front, on a stream made for such stills. It is decoded by itself, in software,
    // at its own size, with its colours. (Before: what is neither a JPEG nor a PNG was refused.)
    void aStillMayBeAnH264KeyFrame()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::KeyFrameStill);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        const std::vector<std::byte> key = keyFrameOf("desktop-1080p30.h264", 0);
        QVERIFY2(!key.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        QVERIFY(key.size() > 5 && key[3] == std::byte{1} && unitsOf(key).front().type == 7);
        stream.push({key, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        const engine::Stats stats = stream.stats();
        QCOMPARE(stats.picture, (engine::SizePx{1920, 1080}));
        QCOMPARE(stats.decoding, engine::Decoding::InSoftware);
        QCOMPARE(stats.refused, std::uint64_t{0});
        QVERIFY(scene->draw(canvas.value()->target()));
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 400, 200), ground, 8);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 8);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 91, 97), red, 12);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 149, 97), green, 12);
        // And the pattern's, at its size.
        engine::Stream& pattern = scene->add(2, engine::Codec::KeyFrameStill);
        pattern.push({keyFrameOf("pattern-1080p30.h264", 0), base::steadyNowUs(), true});
        QTRY_COMPARE(pattern.stats().decoded, std::uint64_t{1});
        QCOMPARE(pattern.stats().picture, (engine::SizePx{1920, 1080}));
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // Two cards fed key frames by turns, a 720p and a 1080p, each keep their own size. (That no picture
    // of one comes out for the other, as a decoder they shared could hand over once FFmpeg's reorder
    // delay rose, is held where the cards' stills come from one connection each, in 2g.)
    void keyFrameCardsKeepTheirOwnSizes()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& narrow = scene->add(1, engine::Codec::KeyFrameStill);
        engine::Stream& wide = scene->add(2, engine::Codec::KeyFrameStill);
        for (std::size_t round = 0; round < 4; ++round) {
            const std::vector<std::byte> one = keyFrameOf("pattern-720p15.h264", round);
            const std::vector<std::byte> other = keyFrameOf("pattern-1080p30.h264", round);
            QVERIFY2(!one.empty() && !other.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
            narrow.push({one, base::steadyNowUs(), true});
            wide.push({other, base::steadyNowUs(), true});
            QTRY_COMPARE(narrow.stats().decoded, std::uint64_t{round + 1});
            QTRY_COMPARE(wide.stats().decoded, std::uint64_t{round + 1});
            QCOMPARE(narrow.stats().picture, (engine::SizePx{1280, 720}));
            QCOMPARE(wide.stats().picture, (engine::SizePx{1920, 1080}));
        }
        QCOMPARE(narrow.stats().refused, std::uint64_t{0});
        QCOMPARE(wide.stats().refused, std::uint64_t{0});
    }

    // Nothing of a key frame's decoder stays after its picture (design 4: 64 cards that kept one each
    // would hold some 640 MB against SPEC.md section 9's 300). Thirty-two cards are each given a 1080p
    // key frame and hold its picture, undrawn: the memory committed to the process grows by those
    // pictures and a little more (some 110 MB for 100 of pictures here), where a decoder kept for each made it
    // some 196. (Not under the address sanitizer, which keeps what is freed for a while.)
    void aKeyFrameStillHoldsNoDecoderAfterIt()
    {
#if defined(__SANITIZE_ADDRESS__)
        QSKIP("The address sanitizer keeps freed memory for a while: what is committed says nothing here.");
#endif
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        std::vector<std::vector<std::byte>> keys;
        for (std::size_t nth = 0; nth < 5; ++nth) {
            keys.push_back(keyFrameOf("desktop-1080p30.h264", nth));
            QVERIFY2(!keys.back().empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        }
        // One first, so that what FFmpeg makes once for every decoder is made before the count begins.
        engine::Stream& first = scene->add(100, engine::Codec::KeyFrameStill);
        first.push({keys[0], base::steadyNowUs(), true});
        QTRY_COMPARE(first.stats().decoded, std::uint64_t{1});
        const auto committed = [] {
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0
                       ? static_cast<double>(counters.PagefileUsage)
                       : 0.0;
        };
        constexpr int cards = 32;
        const double before = committed();
        std::vector<engine::Stream*> streams;
        for (int card = 0; card < cards; ++card) {
            streams.push_back(&scene->add(static_cast<engine::StreamId>(card + 1), engine::Codec::KeyFrameStill));
            streams.back()->push({keys[static_cast<std::size_t>(card) % keys.size()], base::steadyNowUs(), true});
        }
        for (engine::Stream* stream : streams) {
            QTRY_COMPARE(stream->stats().decoded, std::uint64_t{1});
        }
        QTest::qWait(100);
        const double grown = committed() - before;
        const double pictures = 1920.0 * 1080 * 1.5 * cards;
        qInfo() << "grown:" << grown / 1e6 << "MB, the pictures themselves:" << pictures / 1e6 << "MB";
        QVERIFY2(grown < pictures * 1.4,
                 qPrintable(QStringLiteral("%1 MB for %2 MB of pictures").arg(grown / 1e6).arg(pictures / 1e6)));
    }

    // A key frame that cannot be decoded (its SPS and PPS and no picture) breaks that one picture of
    // that one card: the card's next key frame decodes, and the other card never noticed.
    void aBrokenKeyFrameBreaksThatPictureOnly()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& card = scene->add(1, engine::Codec::KeyFrameStill);
        engine::Stream& other = scene->add(2, engine::Codec::KeyFrameStill);
        const std::vector<std::byte> first = keyFrameOf("desktop-720p15.h264", 0);
        const std::vector<std::byte> second = keyFrameOf("desktop-720p15.h264", 1);
        const std::vector<std::byte> third = keyFrameOf("desktop-720p15.h264", 2);
        QVERIFY2(!first.empty() && !third.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        const std::vector<std::byte> broken = headsOnly(second);
        QVERIFY(!broken.empty() && broken.size() < second.size());
        card.push({first, base::steadyNowUs(), true});
        other.push({first, base::steadyNowUs(), true});
        QTRY_COMPARE(card.stats().decoded, std::uint64_t{1});
        QTRY_COMPARE(other.stats().decoded, std::uint64_t{1});
        card.push({broken, base::steadyNowUs(), true});
        other.push({second, base::steadyNowUs(), true});
        QTRY_COMPARE(card.stats().refused, std::uint64_t{1});
        QTRY_COMPARE(other.stats().decoded, std::uint64_t{2});
        QCOMPARE(card.stats().decoded, std::uint64_t{1});
        card.push({third, base::steadyNowUs(), true});
        QTRY_COMPARE(card.stats().decoded, std::uint64_t{2});
        QCOMPARE(card.stats().picture, (engine::SizePx{1280, 720}));
        QCOMPARE(other.stats().refused, std::uint64_t{0});
    }

    // Design 4 (F7): a still byte for byte the one last decoded, while nothing else waits or is being
    // decoded, is let go and counted: it is what is drawn already. One that differs is decoded, and so
    // is the first again after it. A JPEG and a key frame alike.
    void aStillByteForByteTheLastDecodedIsLetGo()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::KeyFrameStill);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{1});
        QCOMPARE(stream.stats().pushed, std::uint64_t{2});
        QTest::qWait(150);
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
        pushFile(stream, made("desktop-preview-480/002.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{3});
        const std::vector<std::byte> key = keyFrameOf("desktop-720p15.h264", 0);
        stream.push({key, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{4});
        stream.push({key, base::steadyNowUs(), true});
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{2});
        QTest::qWait(150);
        QCOMPARE(stream.stats().decoded, std::uint64_t{4});
        QCOMPARE(stream.stats().droppedBehind, std::uint64_t{0});
    }

    // ... and only then: one that comes while another waits or is being decoded is the newest, and is
    // decoded, so the card ends on it. Twenty times: a 1080p key frame (long to decode), then the 480 by
    // 270 JPEG decoded last before it, after none to three milliseconds. (Judged against the last one
    // handed in, or whatever waits, or with one being decoded, the second would be let go, and the card
    // would end on the first.)
    void theStillLastDecodedComingAgainAfterAnotherIsDecoded()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::KeyFrameStill);
        const base::Result<std::vector<std::byte>> narrow = bench::readFile(made("desktop-preview-480/001.jpg"));
        const std::vector<std::byte> wide = keyFrameOf("desktop-1080p30.h264", 0);
        QVERIFY2(narrow.ok() && !wide.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        stream.push({narrow.value(), base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        for (int round = 0; round < 20; ++round) {
            stream.push({wide, base::steadyNowUs(), true});
            const std::int64_t until = base::steadyNowUs() + std::int64_t{round % 4} * 1'000;
            while (base::steadyNowUs() < until) {
            }
            stream.push({narrow.value(), base::steadyNowUs(), true});
            QTest::qWait(150);
            QCOMPARE(stream.stats().picture, (engine::SizePx{480, 270}));
        }
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{0});
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
    }

    // A still that could not be decoded never counts as the last one decoded: the same again is tried
    // again, not let go.
    void aStillWhoseDecodingFailedIsTriedAgain()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::KeyFrameStill);
        const std::vector<std::byte> broken = headsOnly(keyFrameOf("desktop-720p15.h264", 0));
        QVERIFY2(!broken.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        stream.push({broken, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        stream.push({broken, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{2});
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{0});
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        stream.push({broken, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{3});
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{1});
    }

    // After a lost device a still's last picture is decoded again, and the same still coming right after
    // does not leave the card empty: the picture is there again, as it was, and drawn. (That the same
    // still never clears a redraw that still waits is held with a pool of two kept busy, in 2d.)
    void theSameStillAfterALostDeviceIsDrawnAgain()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        for (int round = 0; round < 3; ++round) {
            pushFile(stream, made("desktop-preview-480/001.jpg"));
            QTRY_VERIFY(stream.stats().decoded >= 1);
            QTest::qWait(50);
            const std::uint64_t decoded = stream.stats().decoded;
            QVERIFY(scene->draw(canvas.value()->target()) || round > 0);
            scene->shown(base::steadyNowUs());
            gpu->loseDevice();
            QVERIFY(!scene->draw(canvas.value()->target()));
            pushFile(stream, made("desktop-preview-480/001.jpg"));
            QTRY_VERIFY(stream.stats().decoded > decoded);
            // (QTRY_VERIFY asks once more after it saw a yes, and a draw with nothing new says no.)
            bool drew = false;
            const auto drawn = [&] {
                drew = drew || scene->draw(canvas.value()->target());
                return drew;
            };
            QTRY_VERIFY(drawn());
            const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
            QVERIFY(pixels.ok());
            COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        }
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // M6 2d (design 4, review 6's 2): a pool of two, twenty cards' 1080p key frames waiting on its one
    // thread for previews, and a machine shown large hands in a picture: it is decoded at once, on the
    // thread previews never have, while most of the cards still wait; and the cards all come in the end.
    void aShownLargeStreamIsDecodedBeforeBusyPreviews()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu(2);
        QVERIFY(gpu != nullptr);
        std::vector<engine::Stream*> cards;
        // Counted the moment the large picture is ready (the host's own callback for it, on the decoding
        // thread): how many cards were decoded before it.
        std::atomic<int> before{-1};
        const auto countThem = [&] {
            int unknown = -1;
            const auto now = static_cast<int>(
                std::ranges::count_if(cards, [](engine::Stream* card) { return card->stats().decoded > 0; }));
            before.compare_exchange_strong(unknown, now);
        };
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({{}, {}, countThem});
        const std::vector<std::byte> heavy = keyFrameOf("pattern-1080p30.h264", 0);
        QVERIFY2(!heavy.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        for (int card = 0; card < 20; ++card) {
            cards.push_back(&scene->add(static_cast<engine::StreamId>(100 + card), engine::Codec::KeyFrameStill));
            cards.back()->push({heavy, base::steadyNowUs(), true});
        }
        engine::Stream& large = scene->add(1, engine::Codec::Still);
        large.shownAt({1280, 720}, engine::Importance::ShownLarge);
        pushFile(large, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(large.stats().decoded, std::uint64_t{1});
        QTRY_VERIFY(before.load() >= 0);
        qInfo() << "cards decoded when the large one was:" << before.load() << "of" << cards.size();
        QVERIFY2(before.load() < 10, qPrintable(QString::number(before.load())));
        for (engine::Stream* card : cards) {
            QTRY_VERIFY_WITH_TIMEOUT(card->stats().decoded > 0, 10000);
        }
    }

    // M6 2d: the same still coming while the redraw after a lost device still waits takes nothing from it.
    // A pool of two, whose one thread for previews has twenty other cards' 2160p key frames before the
    // redraw: the still comes while that waits, takes its place, and is drawn. (A still let go as the
    // same that cleared what waited would leave the card empty: 2c-10, caught here.)
    void aRedrawThatWaitsIsNotClearedByTheSameStill()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu(2);
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        const std::vector<std::byte> heavy = keyFrameOf("desktop-2160p60.h264", 0);
        QVERIFY2(!heavy.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        std::vector<engine::Stream*> others;
        others.reserve(20);
        for (int other = 0; other < 20; ++other) {
            others.push_back(&scene->add(static_cast<engine::StreamId>(100 + other), engine::Codec::KeyFrameStill));
        }
        // (Handed their key frames the other way round from the order they were added in. A device made anew takes
        // each stream's decoding in turn, in that order, and waits for the one being decoded; where the preview
        // thread works through them in the same order, the drawing thread follows it from one to the next and waits
        // for every one of them, the redraw is put in a line that is empty by then and made at once, and the same
        // still that comes after it is the picture that waits to be drawn, which push lets go, rightly: in about
        // half of the runs in Release with the stream's steps logged, twice in gate 29's. The other way round, it
        // waits for the one being decoded and the redraw has the rest before it, which 2160p makes some half a
        // second, against the moments the device takes to be made anew.)
        for (auto other = others.rbegin(); other != others.rend(); ++other) {
            (*other)->push({heavy, base::steadyNowUs(), true});
        }
        gpu->loseDevice();
        QVERIFY(!scene->draw(canvas.value()->target()));
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{0});
        QTRY_VERIFY_WITH_TIMEOUT(stream.stats().decoded >= 2, 10000);
        bool drew = false;
        const auto drawn = [&] {
            drew = drew || scene->draw(canvas.value()->target());
            return drew;
        };
        QTRY_VERIFY(drawn());
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        for (engine::Stream* other : others) {
            QTRY_VERIFY_WITH_TIMEOUT(other->stats().decoded >= 1, 10000);
        }
    }

    // 2c's reading, L4: a card taken away while its key frame is being decoded waited, on the thread that
    // takes it away (the interface thread), for that decoding to end: measured for the pattern's 1080p
    // and 2160p key frames and an 8 MB packet (the 2160p one with filler behind it), 9.8, 30.7 and 44.6
    // ms at the longest of five each. Since 2d the pool ends it once its decoding is over, and taking it
    // away waits for nothing: the longest of five each is said, and held under 10 ms.
    void takingACardAwayWhileItsKeyFrameDecodesWaitsSoLong()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        std::vector<std::byte> eight = keyFrameOf("pattern-2160p60.h264", 0);
        const std::size_t real = eight.size();
        for (const std::uint8_t byte :
             {std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{0x0C}}) {
            eight.push_back(static_cast<std::byte>(byte));
        }
        eight.resize(std::size_t{8} * 1024 * 1024 - 64, std::byte{0xFF});
        eight.push_back(std::byte{0x80});
        const struct {
            const char* name;
            std::vector<std::byte> key;
        } kinds[] = {{"1080p", keyFrameOf("pattern-1080p30.h264", 0)},
                     {"2160p", keyFrameOf("pattern-2160p60.h264", 0)},
                     {"8 MB", eight}};
        engine::StreamId number = 1;
        for (const auto& kind : kinds) {
            QVERIFY2(!kind.key.empty() && real > 0, "tools\\make_testdata.ps1 makes the pictures this test needs");
            double longest = 0;
            for (int attempt = 0; attempt < 5; ++attempt) {
                engine::Stream& card = scene->add(number, engine::Codec::KeyFrameStill);
                card.push({kind.key, base::steadyNowUs(), true});
                // (Long enough for a decoding thread to have begun it, short of its end.)
                const std::int64_t until = base::steadyNowUs() + 2'000;
                while (base::steadyNowUs() < until) {
                }
                const std::int64_t began = base::steadyNowUs();
                scene->remove(number++);
                longest = std::max(longest, static_cast<double>(base::steadyNowUs() - began) / 1000.0);
            }
            qInfo() << kind.name << "key frame (" << kind.key.size() / 1024 << "KB): taking its card away waited"
                    << longest << "ms at the longest";
            QVERIFY2(longest < 10, qPrintable(QStringLiteral("%1 ms").arg(longest)));
        }
    }

    // 2d's reading (L4): a stream taken away decodes nothing more of what waited for it, and is urgent no more:
    // a machine shown large with eight 2160p key frames waiting is taken away as one decodes; the scene ends
    // once that one is over, and the host is told nothing as to be drawn at once after it was taken away.
    // (Before: what waited was decoded still, and told of as urgent, once in every round.)
    void aStreamTakenAwayDecodesNothingMoreOfWhatWaited()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::vector<std::byte> key = keyFrameOf("pattern-2160p60.h264", 0);
        QVERIFY2(!key.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        // Five rounds: told as urgent (the host's own callback for that), nothing after it was taken away.
        for (int round = 0; round < 5; ++round) {
            std::atomic<int> told{0};
            auto scene = gpu->makeScene({{}, {}, [&told] { ++told; }});
            engine::Stream& large = scene->add(1, engine::Codec::H264);
            large.shownAt({3840, 2160}, engine::Importance::ShownLarge);
            large.push({key, base::steadyNowUs(), true});
            QTRY_COMPARE(large.stats().decoded, std::uint64_t{1});
            for (int each = 0; each < 8; ++each) {
                large.push({key, base::steadyNowUs(), true});
            }
            const std::int64_t until = base::steadyNowUs() + 2'000;
            while (base::steadyNowUs() < until) {
            }
            const int before = told.load();
            QElapsedTimer ending;
            ending.start();
            scene->remove(1);
            scene.reset();
            qInfo() << "round" << round << ": told" << told.load() - before
                    << "times after it was taken away; the scene"
                    << "ended" << ending.elapsed() << "ms after";
            QCOMPARE(told.load(), before);
        }
    }

    // Measured for 2d's reading: right after a lost device, sixty-four cards' last stills are decoded again
    // and copied to the card in the draw that finds them, under the device's lock: how long that draw
    // takes is how long a machine shown large waits then. Said, not held.
    void sixtyFourCardsRedrawnAfterALostDeviceTakeOneDrawSoLong()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{1920, 1080};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        std::vector<engine::Placed> placed;
        std::vector<engine::Stream*> cards;
        for (int card = 0; card < 64; ++card) {
            cards.push_back(&scene->add(static_cast<engine::StreamId>(card + 1), engine::Codec::Still));
            placed.push_back({static_cast<engine::StreamId>(card + 1), {(card % 8) * 240, (card / 8) * 135, 240, 135}});
            pushFile(*cards.back(), made("desktop-preview-480/001.jpg"));
        }
        scene->place(placed);
        for (engine::Stream* card : cards) {
            QTRY_VERIFY(card->stats().decoded >= 1);
        }
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        gpu->loseDevice();
        static_cast<void>(scene->draw(canvas.value()->target()));
        for (engine::Stream* card : cards) {
            QTRY_VERIFY(card->stats().decoded >= 2);
        }
        QElapsedTimer drawing;
        drawing.start();
        QVERIFY(scene->draw(canvas.value()->target()));
        qInfo() << "the first draw of sixty-four cards after a lost device took" << drawing.elapsed() << "ms";
    }

    // ... and a scene that ends while a card it took away still decodes waits for that one: nothing of
    // the card reaches the scene's host after the scene is gone (an address sanitizer run says the rest).
    void aSceneThatEndsWaitsForTheCardsItTookAway()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::vector<std::byte> key = keyFrameOf("pattern-2160p60.h264", 0);
        QVERIFY2(!key.empty(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        std::atomic<int> told{0};
        for (int round = 0; round < 5; ++round) {
            auto scene = gpu->makeScene({[&] { ++told; }, {}, {}});
            engine::Stream& card = scene->add(1, engine::Codec::KeyFrameStill);
            card.push({key, base::steadyNowUs(), true});
            const std::int64_t until = base::steadyNowUs() + 2'000;
            while (base::steadyNowUs() < until) {
            }
            scene->remove(1);
            scene.reset();
            const int afterTheEnd = told.load();
            QTest::qWait(100);
            QCOMPARE(told.load(), afterTheEnd);
        }
    }

    // 2c, after its reading (M1): a still decoded and never drawn (its copy to the card failed, as where
    // the card's memory is full, with the device as it was) is not "the same" when it comes again: it
    // is decoded again, and the card heals at the next one alike. (Before: let go every time, and the
    // card stayed old or empty until the machine's screen changed.)
    void aStillWhoseCopyFailedIsDecodedAgainWhenItComesAgain()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{480, 270};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        gpu->failTextures(1);
        static_cast<void>(scene->draw(canvas.value()->target()));
        QCOMPARE(stream.stats().refused, std::uint64_t{1});
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{0});
        QVERIFY(scene->draw(canvas.value()->target()));
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 150, 140), panel, 6);
        // Drawn now: the same again is let go.
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QCOMPARE(stream.stats().stillsSame, std::uint64_t{1});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // ... and the copy that failed leaves nothing drawn that went with it: the planes of the picture
    // before (of another size) were let go to make new ones, and the frame draws an empty tile, not
    // views that are gone. (Before: the tile was drawn from them, with what they had become.)
    void aCopyThatFailsLeavesNothingDrawnThatWentWithIt()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{640, 360};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        pushFile(stream, made("desktop-preview-640-422/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        gpu->failTextures(1);
        QVERIFY(scene->draw(canvas.value()->target()));
        QCOMPARE(stream.stats().refused, std::uint64_t{1});
        base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 200, 190), black, 0);
        VERIFY_NO_OBJECTIONS(*gpu);
        // And it heals at the same still again.
        pushFile(stream, made("desktop-preview-640-422/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{3});
        QVERIFY(scene->draw(canvas.value()->target()));
        QCOMPARE(stream.stats().picture, (engine::SizePx{640, 360}));
        pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 200, 190), panel, 8);
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    // A still is kept as the last decoded before the host is told of its picture: the same still handed
    // in from inside that telling (the host's own callback, on the decoding thread) is let go. (Kept only
    // after, it would be decoded again: no race of microseconds decides this here.)
    void whoeverIsToldOfAStillFindsItTheLastDecoded()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const base::Result<std::vector<std::byte>> jpeg = bench::readFile(made("desktop-preview-480/001.jpg"));
        QVERIFY2(jpeg.ok(), "tools\\make_testdata.ps1 makes the pictures this test needs");
        engine::Stream* stream = nullptr;
        std::atomic<int> told{0};
        const std::unique_ptr<engine::Scene> scene =
            gpu->makeScene({[&] {
                                if (told++ == 0 && stream != nullptr) {
                                    stream->push({jpeg.value(), base::steadyNowUs(), true});
                                }
                            },
                            {},
                            {}});
        stream = &scene->add(1, engine::Codec::Still);
        stream->push({jpeg.value(), base::steadyNowUs(), true});
        QTRY_COMPARE(told.load(), 1);
        QTest::qWait(100);
        QCOMPARE(stream->stats().pushed, std::uint64_t{2});
        QCOMPARE(stream->stats().stillsSame, std::uint64_t{1});
        QCOMPARE(stream->stats().decoded, std::uint64_t{1});
    }

    void aPictureThatClaimsToBeHugeTakesNoMemory()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::Still);

        // A few bytes that say they are a picture of 16000 by 16000: three quarters of a gigabyte,
        // if a decoder believed them.
        const std::size_t before = mostEverCommitted();
        const std::vector<std::byte> claim = pngThatClaims(16000, 16000);
        stream.push({claim, base::steadyNowUs(), true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        QCOMPARE(stream.stats().decoded, std::uint64_t{0});
        const std::size_t after = mostEverCommitted();
        QVERIFY2(
            after - before < std::size_t{64} * 1024 * 1024,
            qPrintable(QStringLiteral("%1 MB were taken for it").arg((after - before) / (std::size_t{1024} * 1024))));

        // And the stream goes on with the next picture that is one.
        pushFile(stream, made("desktop-stills-png/rgb.png"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
    }

    void whatIsNotAPictureIsRefused()
    {
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        std::atomic<int> failures{0};
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene(
            {{}, [&](engine::Fault fault) { failures += fault == engine::Fault::StreamFailed ? 1 : 0; }, {}});
        engine::Stream& stream = scene->add(1, engine::Codec::Still);

        const std::vector<std::byte> nothing;
        stream.push({nothing, 0, true});
        QCOMPARE(stream.stats().refused, std::uint64_t{1});

        const std::vector<std::byte> rubbish(4096, std::byte{0x5a});
        stream.push({rubbish, 0, true});
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{2});
        QTRY_COMPARE(failures.load(), 1);
        QCOMPARE(stream.stats().decoded, std::uint64_t{0});

        // And the stream goes on with the next picture that is one.
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
    }

    void aServersScreenIsDrawnFromWhatChanged()
    {
        // (M7 3d.) A server's screen comes whole once, and then as what changed of it: each change is drawn onto the
        // screen as it stands, colour for colour, and only what changed is copied to the card.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        std::vector<std::uint8_t> rgb = halves(size.width, size.height, Colour{255, 255, 255}, black);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QCOMPARE(stream.stats().picture, size);
        QCOMPARE(stream.stats().decoding, engine::Decoding::InSoftware);
        std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 8, 8), (Colour{255, 255, 255}), 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 20), black, 0);
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{3} * 64 * 32);
        // One block turns red, another amber: those, and nothing else, change and are copied.
        pushBytes(stream, painted(rgb, size.width, size.height, {{48, 16, 8, 8}, {0, 0, 8, 8}}, red));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 52, 20), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 4, 4), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 8, 8), (Colour{255, 255, 255}), 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 20), black, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 47, 16), black, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 56, 24), black, 0);
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{3} * 64 * 32 + std::uint64_t{2} * 3 * 8 * 8);
        // A screen that does not change sends nothing, and nothing is copied or drawn anew.
        QVERIFY(!scene->draw(canvas.value()->target()));
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{3} * 64 * 32 + std::uint64_t{2} * 3 * 8 * 8);
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
        QVERIFY(!stream.takeKeyFrameWish());
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void changesThatCameTogetherAreAllDrawn()
    {
        // (M7 3d.) Changes that come faster than they are drawn are none of them lost: each is applied as it comes,
        // and the next draw copies what all of them changed. Too many rectangles of them are copied as the whole.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        std::vector<std::uint8_t> rgb = halves(size.width, size.height, black, black);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(!drawnInto(*scene, *canvas.value()).empty());
        const std::uint64_t first = stream.stats().uploadedBytes;
        pushBytes(stream, painted(rgb, size.width, size.height, {{0, 0, 8, 8}}, red));
        pushBytes(stream, painted(rgb, size.width, size.height, {{56, 24, 8, 8}}, green));
        pushBytes(stream, painted(rgb, size.width, size.height, {{4, 4, 8, 8}}, amber));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{4});
        std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 1, 1), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 60, 28), green, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 6, 6), amber, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 30, 16), black, 0);
        QCOMPARE(stream.stats().uploadedBytes, first + std::uint64_t{3} * 3 * 8 * 8);
        QCOMPARE(stream.stats().replaced, std::uint64_t{2});
        // Seventy changes of a pixel each before the next draw (each applied as it came): the whole screen is copied
        // once.
        for (int one = 0; one < 70; ++one) {
            pushBytes(stream, painted(rgb, size.width, size.height, {{one % 64, one / 64, 1, 1}}, green));
            QTRY_COMPARE(stream.stats().decoded, static_cast<std::uint64_t>(5 + one));
        }
        const std::uint64_t before = stream.stats().uploadedBytes;
        pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 5, 1), green, 0);
        QCOMPARE(stream.stats().uploadedBytes, before + std::uint64_t{3} * 64 * 32);
        QCOMPARE(stream.stats().droppedBehind, std::uint64_t{0});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void aScreenThatChangesFasterThanItIsAppliedIsBroughtForward()
    {
        // (M7 3d.) Changes that come faster than they are applied: a few wait; beyond that, what waits goes (none of
        // it is the whole screen), the stream waits for a whole screen and wishes for one, and is right after it.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu(1);
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        std::vector<std::uint8_t> rgb = halves(size.width, size.height, black, black);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        // (Made first, and handed in at once: faster than they are applied, also where the test runs slow.)
        std::vector<std::vector<std::byte>> changes;
        changes.reserve(200);
        for (int one = 0; one < 200; ++one) {
            changes.push_back(painted(rgb, size.width, size.height, {{one % 64, one / 64, 1, 1}}, red));
        }
        for (const std::vector<std::byte>& change : changes) {
            pushBytes(stream, change);
        }
        QTRY_VERIFY(stream.stats().droppedBehind > 0);
        // (The decoding counts what it let go of before it wishes for a whole one, on its own thread: the wish is
        // waited for too, by qWaitFor, which asks no more once it holds: the wish is taken by asking.)
        QVERIFY(QTest::qWaitFor([&stream] { return stream.takeKeyFrameWish(); }, 5000));
        const std::uint64_t decoded = stream.stats().decoded;
        pushBytes(stream, painted(rgb, size.width, size.height, {{10, 20, 1, 1}}, red)); // (not after what was lost)
        QTest::qWait(100);
        QCOMPARE(stream.stats().decoded, decoded);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, decoded + 1);
        const std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 10, 2), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 10, 20), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 20), black, 0);
    }

    void whatChangedOfAScreenNotHeldWaitsForAWholeOne()
    {
        // (M7 3d.) Changes before the first whole screen, of a screen of another size, or of what is no packet, cannot
        // be drawn onto anything right: they are let go of, and a whole screen is wished for and waited for.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        std::vector<std::uint8_t> rgb = halves(64, 32, black, black);
        pushBytes(stream, painted(rgb, 64, 32, {{0, 0, 8, 8}}, red));
        QCOMPARE(stream.stats().droppedBehind, std::uint64_t{1});
        QVERIFY(stream.takeKeyFrameWish());
        QCOMPARE(stream.stats().decoded, std::uint64_t{0});
        pushBytes(stream, base::screen::whole(64, 32, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        // Of another size: let go of.
        std::vector<std::uint8_t> other = halves(32, 16, black, black);
        pushBytes(stream, painted(other, 32, 16, {{0, 0, 8, 8}}, red));
        QTRY_COMPARE(stream.stats().droppedBehind, std::uint64_t{2});
        // (Counted before the wish, on the decoding's thread: the wish is waited for too, as above.)
        QVERIFY(QTest::qWaitFor([&stream] { return stream.takeKeyFrameWish(); }, 5000));
        pushBytes(stream, painted(rgb, 64, 32, {{0, 0, 8, 8}}, red)); // (and what comes after it, before a whole one)
        QCOMPARE(stream.stats().droppedBehind, std::uint64_t{3});
        // ...and a whole one of the other size is that screen from now on.
        pushBytes(stream, base::screen::whole(32, 16, other));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        QCOMPARE(stream.stats().picture, (engine::SizePx{32, 16}));
        // What is no packet is refused, and a whole screen wished for.
        const std::vector<std::byte> rubbish(64, std::byte{0x5a});
        const base::screen::Rect corner{0, 0, 4, 4};
        std::vector<std::byte> notQuite = base::screen::write(32, 16, other, std::span(&corner, 1));
        notQuite.pop_back();
        pushBytes(stream, notQuite);
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        QVERIFY(QTest::qWaitFor([&stream] { return stream.takeKeyFrameWish(); },
                                5000)); // (counted before the wish, as above)
        pushBytes(stream, rubbish);
        QCOMPARE(stream.stats().decoded, std::uint64_t{2});
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void aServersScreenIsDrawnAgainFromMemoryAfterALostDevice()
    {
        // (M7 3d.) The screen as it stands is in this program's memory: after a lost device it is copied whole to the
        // device that comes back, and nothing is asked of the server for it.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{64, 32};
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        std::vector<std::uint8_t> rgb = halves(size.width, size.height, red, green);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(scene->draw(canvas.value()->target()));
        scene->shown(base::steadyNowUs());
        const std::uint64_t copied = stream.stats().uploadedBytes;
        gpu->loseDevice();
        QVERIFY(!scene->draw(canvas.value()->target()));
        // (QTRY_VERIFY asks once more after it saw a yes, and a draw with nothing new says no.)
        bool drew = false;
        const auto drawn = [&] {
            drew = drew || scene->draw(canvas.value()->target());
            return drew;
        };
        QTRY_VERIFY(drawn());
        QVERIFY(gpu->timesRecovered() > 0);
        const base::Result<std::vector<std::uint8_t>> read = canvas.value()->pixels();
        QVERIFY(read.ok());
        const std::vector<std::uint8_t>& pixels = read.value();
        QCOMPARE(stream.stats().uploadedBytes, copied + std::uint64_t{3} * 64 * 32);
        COMPARE_COLOUR(colourAt(pixels, size, 8, 8), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 8), green, 0);
        QVERIFY(!stream.takeKeyFrameWish());
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
        // Drawn again, not arrived (after its reading, low 8): shown, and no time from an arrival counted for it.
        const auto arrivals = [&stream] {
            const engine::Stats stats = stream.stats();
            return std::accumulate(stats.arrivedToShownCounts.begin(), stats.arrivedToShownCounts.end(),
                                   std::uint64_t{0});
        };
        const std::uint64_t before = arrivals();
        QCOMPARE(before, std::uint64_t{1});
        scene->shown(base::steadyNowUs());
        QCOMPARE(stream.stats().shown, std::uint64_t{2});
        QCOMPARE(arrivals(), before);
    }

    void aScreenBeingAppliedDoesNotHoldUpTheDrawing()
    {
        // (M7 3d, after its reading, M1.) A packet is being applied to a server's screen, which takes long for a busy
        // one (here a second, by the device's own slowApplying): the drawing, with the device's lock held, does not
        // wait for it (every other stream's drawing, and the card's decoding, would). It draws the screen as it was
        // copied last, and what is applied by then is drawn at the next draw after it.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu(2);
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{128, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& server = scene->add(1, engine::Codec::Rgb);
        engine::Stream& kvm = scene->add(2, engine::Codec::Still);
        const engine::Placed both[] = {{1, {0, 0, 64, 32}}, {2, {64, 0, 64, 32}}};
        scene->place(both);
        std::vector<std::uint8_t> rgb = halves(64, 32, red, red);
        pushBytes(server, base::screen::whole(64, 32, rgb));
        pushFile(kvm, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(server.stats().decoded, std::uint64_t{1});
        QTRY_COMPARE(kvm.stats().decoded, std::uint64_t{1});
        QVERIFY(!drawnInto(*scene, *canvas.value()).empty());
        // A change applied and not drawn yet; then one that takes a second to apply.
        pushBytes(server, painted(rgb, 64, 32, {{0, 0, 8, 8}}, green));
        QTRY_COMPARE(server.stats().decoded, std::uint64_t{2});
        gpu->slowApplying(1000);
        pushBytes(server, painted(rgb, 64, 32, {{8, 0, 8, 8}}, amber));
        QTest::qWait(200); // (taken by the decoding, which holds the screen now)
        QCOMPARE(server.stats().decoded, std::uint64_t{2});
        QElapsedTimer drawing;
        drawing.start();
        (void)scene->draw(canvas.value()->target()); // (nothing new to draw by then, maybe: what is up stays)
        const qint64 took = drawing.elapsed();
        QVERIFY2(took < 400, QByteArray::number(took).constData());
        const base::Result<std::vector<std::uint8_t>> during = canvas.value()->pixels();
        QVERIFY(during.ok());
        COMPARE_COLOUR(colourAt(during.value(), size, 2, 2), red, 0); // (as copied last)
        gpu->slowApplying(0);
        QTRY_COMPARE_WITH_TIMEOUT(server.stats().decoded, std::uint64_t{3}, 5000);
        const std::vector<std::uint8_t> after = drawnInto(*scene, *canvas.value());
        QVERIFY(!after.empty());
        COMPARE_COLOUR(colourAt(after, size, 2, 2), green, 0);
        COMPARE_COLOUR(colourAt(after, size, 10, 2), amber, 0);
        COMPARE_COLOUR(colourAt(after, size, 30, 20), red, 0);
        VERIFY_NO_OBJECTIONS(*gpu);
    }

    void aScreenWhoseCopyFailedIsCopiedWholeWithItsNextChange()
    {
        // (M7 3d.) The card could not take the copy (its memory full, say): nothing that went with it is drawn, and the
        // next change copies the whole screen, which then is right.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        std::vector<std::uint8_t> rgb = halves(size.width, size.height, red, green);
        gpu->failTextures(1);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        (void)scene->draw(canvas.value()->target());
        QCOMPARE(stream.stats().refused, std::uint64_t{1});
        pushBytes(stream, painted(rgb, size.width, size.height, {{0, 0, 8, 8}}, amber));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        const std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 2, 2), amber, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 20, 20), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 20), green, 0);
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{3} * 64 * 32);
    }

    void aScreenWhoseCopyFailedIsDrawnAtTheNextDraw()
    {
        // (M7 3d, after its reading, M2.) The same, where the server's screen changes no more (one at its BIOS, or a
        // login prompt, sends nothing): the next draw copies it whole all the same, without a change or a wish
        // (before, it stayed black until the screen changed). And that draw is asked of the host, a moment after the
        // copy failed (the outside review of 2026-11-05, F4: it was left for a draw that came for another reason,
        // and none may come).
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        std::atomic<int> asked{0};
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({[&] { ++asked; }, {}, {}});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        const std::vector<std::uint8_t> rgb = halves(size.width, size.height, red, green);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(!drawnInto(*scene, *canvas.value()).empty());
        // Copied whole again after a lost device, and that copy fails.
        gpu->loseDevice();
        QVERIFY(!scene->draw(canvas.value()->target()));
        gpu->failTextures(1);
        // (Drawn until the copy has failed once, and not once more: QTRY_VERIFY asks again after it saw a yes.)
        bool failed = false;
        const auto failedOnce = [&] {
            if (!failed) {
                (void)scene->draw(canvas.value()->target());
                failed = stream.stats().refused > 0;
            }
            return failed;
        };
        QTRY_VERIFY(failedOnce());
        QCOMPARE(stream.stats().refused, std::uint64_t{1});
        asked = 0;
        QTRY_VERIFY(asked.load() > 0);
        const std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 8, 8), red, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 8), green, 0);
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
        QVERIFY(!stream.takeKeyFrameWish());
    }

    void aScreenWhoseCopyGoesOnFailingIsTriedAtAPaceAndDrawnOnceItCanBe()
    {
        // (The outside review of 2026-11-05, F4.) A server's screen comes once and changes no more, and the card
        // cannot take its copy for a while (its memory full). The host draws when it is asked to, and at no other
        // time; no second packet comes. The copy is tried again at draws the engine asks for itself: not at every
        // frame (50 ms after the first failure, twice as long after each one in a row, a second at the most), and
        // the screen is drawn at the first try after the card can take it again. Then nothing more is asked.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        std::atomic<int> asked{0};
        const auto ask = [&] { ++asked; };
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({ask, {}, ask});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        gpu->failTextures(std::numeric_limits<int>::max());
        pushBytes(stream, base::screen::whole(size.width, size.height, halves(size.width, size.height, red, green)));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QTRY_VERIFY(asked.load() > 0);
        // The host: a draw for every asking, as it comes.
        int answered = 0;
        const auto hostFor = [&](qint64 ms, const std::function<bool()>& done) {
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < ms && !done()) {
                for (; answered < asked.load(); ++answered) {
                    (void)scene->draw(canvas.value()->target());
                }
                QTest::qWait(5);
            }
        };
        hostFor(2000, [] { return false; });
        // In two seconds: tried at 0, 50, 150, 350, 750 and 1550 ms (at every frame it would be some 120 times).
        const std::uint64_t tried = stream.stats().refused;
        QVERIFY2(tried >= 3 && tried <= 10, QByteArray::number(tried).constData());
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{0});
        // The card can take it again: at the next try, within a second.
        gpu->failTextures(0);
        QElapsedTimer since;
        since.start();
        hostFor(3000, [&] { return stream.stats().uploadedBytes != 0; });
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{3} * 64 * 32);
        QVERIFY2(since.elapsed() < 2000, QByteArray::number(since.elapsed()).constData());
        const base::Result<std::vector<std::uint8_t>> pixels = canvas.value()->pixels();
        QVERIFY(pixels.ok());
        COMPARE_COLOUR(colourAt(pixels.value(), size, 8, 8), red, 0);
        COMPARE_COLOUR(colourAt(pixels.value(), size, 40, 8), green, 0);
        QCOMPARE(stream.stats().pushed, std::uint64_t{1});
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
        // Drawn: nothing more is asked.
        hostFor(300, [] { return false; });
        const int then = asked.load();
        QTest::qWait(1200);
        QCOMPARE(asked.load(), then);
    }

    void theLargestScreenOfAServerIsDrawn()
    {
        // (M7 3d.) A controller's largest screen, 2048 by 2048, whole: twelve megabytes, more than a packet of video
        // may be, and what a screen of the kind may be.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{512, 512};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Rgb);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        const std::vector<std::uint8_t> rgb = halves(2048, 2048, amber, black);
        const std::vector<std::byte> packet = base::screen::whole(2048, 2048, rgb);
        QVERIFY(packet.size() > engine::maxPacketBytes);
        pushBytes(stream, packet);
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        QCOMPARE(stream.stats().picture, (engine::SizePx{2048, 2048}));
        const std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 100, 256), amber, 2);
        COMPARE_COLOUR(colourAt(pixels, size, 400, 256), black, 2);
        QCOMPARE(stream.stats().refused, std::uint64_t{0});
    }

    void aWholeScreenIsAStill()
    {
        // (M7 3d.) A server's card: its still is the whole screen, in the same packet, and drawn as a still is. What
        // changed of a screen is no still.
        const std::unique_ptr<engine::Gpu> gpu = softwareGpu();
        QVERIFY(gpu != nullptr);
        const std::unique_ptr<engine::Scene> scene = gpu->makeScene({});
        const engine::SizePx size{64, 32};
        base::Result<std::unique_ptr<engine::Canvas>> canvas = gpu->makeCanvas(size);
        QVERIFY(canvas.ok());
        engine::Stream& stream = scene->add(1, engine::Codec::Still);
        const engine::Placed whole[] = {{1, {0, 0, size.width, size.height}}};
        scene->place(whole);
        std::vector<std::uint8_t> rgb = halves(size.width, size.height, titleBar, panel);
        pushBytes(stream, base::screen::whole(size.width, size.height, rgb));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{1});
        const std::vector<std::uint8_t> pixels = drawnInto(*scene, *canvas.value());
        QVERIFY(!pixels.empty());
        COMPARE_COLOUR(colourAt(pixels, size, 8, 8), titleBar, 0);
        COMPARE_COLOUR(colourAt(pixels, size, 40, 20), panel, 0);
        QCOMPARE(stream.stats().uploadedBytes, std::uint64_t{3} * 64 * 32); // (copied from this program's memory)
        pushBytes(stream, painted(rgb, size.width, size.height, {{0, 0, 8, 8}}, red));
        QTRY_COMPARE(stream.stats().refused, std::uint64_t{1});
        QCOMPARE(stream.stats().decoded, std::uint64_t{1});
        // And a JPEG after it, as any still stream takes one.
        pushFile(stream, made("desktop-preview-480/001.jpg"));
        QTRY_COMPARE(stream.stats().decoded, std::uint64_t{2});
        VERIFY_NO_OBJECTIONS(*gpu);
    }
};

QTEST_GUILESS_MAIN(TestScene)
#include "test_scene.moc"
