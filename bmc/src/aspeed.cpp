#include <qkvm/bmc/aspeed.h>

#include <qkvm/bmc/aspeed_format.h>

#include <algorithm>
// Built with the standard library's own checks in every build (bmc/CMakeLists.txt): a build without them is refused
// here, and never loses them without a word.
#if !_MSVC_STL_HARDENING_ARRAY || !_MSVC_STL_HARDENING_SPAN || !_MSVC_STL_HARDENING_VECTOR
#error "This reads what a management controller sends, and is built with _MSVC_STL_HARDENING (bmc/CMakeLists.txt)"
#endif
#include <string>

// As the web's app/qkvm/aspeed.py, step for step: the same bits taken in the same order, the same whole-number
// arithmetic (the web's numbers have no end; here they are 64 bits, which no frame's can reach: see `plane`),
// and so the same picture, byte for byte (tests/test_aspeed.cpp holds it to the web's).
namespace qkvm::bmc::aspeed {

namespace {

// The eight strengths: the standard's table times this many thirty-seconds, the coarsest first.
constexpr std::array<int, 8> lumaStrengths = {40, 35, 29, 23, 18, 12, 6, 4};
constexpr std::array<int, 8> chromaStrengths = {60, 52, 43, 35, 26, 18, 9, 6};

constexpr std::array<int, 64> lumaQuant = {
    16, 11,  10,  16, 24, 40, 51, 61, 12,  12,  14,  19,  26, 58, 60, 55,  14,  13,  16,  24, 40, 57,
    69, 56,  14,  17, 22, 29, 51, 87, 80,  62,  18,  22,  37, 56, 68, 109, 103, 77,  24,  35, 55, 64,
    81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98,  112, 100, 103, 99,
};
constexpr std::array<int, 64> chromaQuant = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

// The inverse transform works on coefficients that were scaled beforehand (Arai, Agui and Nakajima's method,
// as in the JPEG group's own fast decoder): these are the factors, by row and by column.
constexpr std::array<double, 8> scale = {1.0, 1.387039845, 1.306562965, 1.175875602,
                                         1.0, 0.785694958, 0.541196100, 0.275899379};

// A sample as the transform leaves it (ten bits, signed, around zero) as a byte around 128.
constexpr std::array<std::uint8_t, 1024> sampleOf = [] {
    std::array<std::uint8_t, 1024> found{};
    for (int v = 0; v < 1024; ++v) {
        found[static_cast<std::size_t>(v)] =
            static_cast<std::uint8_t>(v < 512 ? std::min(255, 128 + v) : std::max(0, v - 896));
    }
    return found;
}();

// YCbCr (ITU-R 601, as video has it: black at 16) to RGB, in sixteenth-bit fixed point as the sender's own
// decoder does it, and a table that keeps the result between 0 and 255.
constexpr std::array<int, 256> fixedTable(double factor, int from, int plus)
{
    std::array<int, 256> found{};
    const auto times = static_cast<int>(factor * 65536);
    for (int c = 0; c < 256; ++c) {
        // (A shift of a number below zero rounds down, as the web's does.)
        found[static_cast<std::size_t>(c)] = ((times * (c - from) + 32768) >> 16) + plus;
    }
    return found;
}
constexpr std::array<int, 256> lightOf = [] {
    std::array<int, 256> found{};
    for (int y = 0; y < 256; ++y) {
        found[static_cast<std::size_t>(y)] = ((76284 * (y - 16) + 32768) >> 16) + 512; // 512 more: see byteOf
    }
    return found;
}();
constexpr std::array<int, 256> crToR = fixedTable(1.597656, 128, 0);
constexpr std::array<int, 256> cbToB = fixedTable(2.015625, 128, 0);
constexpr std::array<int, 256> crToG = fixedTable(-0.8125, 128, 0);
constexpr std::array<int, 256> cbToG = fixedTable(-0.390625, 128, 0);
constexpr std::array<std::uint8_t, 1600> byteOf = [] {
    std::array<std::uint8_t, 1600> found{};
    for (int v = 0; v < 1600; ++v) {
        found[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(std::min(255, std::max(0, v - 512)));
    }
    return found;
}();

// One pixel from its brightness and its two colour differences, put into the three bytes of `line` from `at`.
void pixel(std::span<std::uint8_t> line, std::size_t at, std::uint8_t y, std::uint8_t cb, std::uint8_t cr) noexcept
{
    const int light = lightOf[y];
    const int red = light + crToR[cr];
    const int green = light + cbToG[cb] + crToG[cr];
    const int blue = light + cbToB[cb];
    line[at] = byteOf[static_cast<std::size_t>(red)];
    line[at + 1] = byteOf[static_cast<std::size_t>(green)];
    line[at + 2] = byteOf[static_cast<std::size_t>(blue)];
}

// For every 16 bits that may come next: the length of the code they begin with, and what it stands for
// ((length << 8) | value; 0 where they begin with no code).
using Lookup = std::vector<std::uint16_t>;
Lookup lookup(const format::Huffman& table)
{
    Lookup found(65536, 0);
    std::uint32_t code = 0;
    std::size_t at = 0;
    for (unsigned length = 1; length <= 16; ++length) {
        for (unsigned one = 0; one < table.counts[length - 1]; ++one) {
            const std::uint32_t first = code << (16 - length);
            std::fill_n(found.begin() + static_cast<std::ptrdiff_t>(first), std::size_t{1} << (16 - length),
                        static_cast<std::uint16_t>((length << 8) | table.values[at]));
            ++code;
            ++at;
        }
        code <<= 1;
    }
    return found;
}

// The four tables, made once for the program and never changed (they are the standard's).
struct Lookups {
    Lookup dcLuma = lookup(format::dcLuma);
    Lookup dcChroma = lookup(format::dcChroma);
    Lookup acLuma = lookup(format::acLuma);
    Lookup acChroma = lookup(format::acChroma);
};
const Lookups& lookups()
{
    static const Lookups made;
    return made;
}

// One strength of a quantization table, with the transform's scaling worked in.
Screen::Quantizers quantizers(const std::array<int, 64>& standard, int thirtySeconds)
{
    Screen::Quantizers found{};
    for (std::size_t at = 0; at < 64; ++at) {
        const int q = std::max(1, std::min(255, standard[at] * thirtySeconds / 32));
        found[at] = static_cast<std::int32_t>(static_cast<double>(q) * scale[at / 8] * scale[at % 8]);
    }
    return found;
}

// What a block is corrected from that was never sent: nothing at all, in the middle of the colours.
const Screen::Kept& nothing()
{
    static const Screen::Kept made = [] {
        Screen::Kept found{};
        found.cb.fill(128);
        found.cr.fill(128);
        return found;
    }();
    return made;
}

base::Error garbled(std::string what)
{
    return {"garbled", std::move(what)};
}

// The bits of a frame, taken from the top of each word down. Past its end a frame reads as nothing: the web's
// has 80 words of nothing after it, which by the format no block reaches the end of (each plane read from
// nothing takes at most 191 bits), and here there is no end to the nothing, which is the same.
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> data) : m_data(data), m_count(data.size() / 4)
    {
        m_acc = (std::uint64_t{word(0)} << 32) | word(1);
    }

    std::size_t words() const noexcept { return m_count; }
    // How many bits were taken.
    std::int64_t taken() const noexcept { return 32 * static_cast<std::int64_t>(m_next) - m_nbits; }
    unsigned peek(int k) const noexcept { return static_cast<unsigned>(m_acc >> (m_nbits - k)) & ((1u << k) - 1); }

    // The next k bits (k from 1 to 32).
    std::uint32_t take(int k) noexcept
    {
        m_nbits -= k;
        const auto value = static_cast<std::uint32_t>(m_acc >> m_nbits);
        m_acc &= (std::uint64_t{1} << m_nbits) - 1;
        if (m_nbits < 32) {
            m_acc = (m_acc << 32) | word(m_next);
            m_nbits += 32;
            ++m_next;
        }
        return value;
    }

private:
    std::uint32_t word(std::size_t at) const noexcept
    {
        if (at >= m_count) {
            return 0;
        }
        const std::span<const std::uint8_t> four = m_data.subspan(4 * at, 4);
        return std::uint32_t{four[0]} | (std::uint32_t{four[1]} << 8) | (std::uint32_t{four[2]} << 16) |
               (std::uint32_t{four[3]} << 24);
    }

    std::span<const std::uint8_t> m_data;
    std::size_t m_count;
    std::uint64_t m_acc = 0;
    int m_nbits = 64;
    std::size_t m_next = 2;
};

using Samples = Screen::Samples;

// The 64 samples of a block from its 64 coefficients: the inverse cosine transform, in whole numbers.
void inverse(const std::array<std::int64_t, 64>& c, const Screen::Quantizers& q, Samples& out) noexcept
{
    std::array<std::int64_t, 64> w{};
    for (std::size_t x = 0; x < 8; ++x) {
        const std::int64_t c1 = c[x + 8], c2 = c[x + 16], c3 = c[x + 24], c4 = c[x + 32], c5 = c[x + 40],
                           c6 = c[x + 48], c7 = c[x + 56];
        if (!(c1 || c2 || c3 || c4 || c5 || c6 || c7)) {
            const std::int64_t only = q[x] * c[x];
            for (std::size_t row = 0; row < 64; row += 8) {
                w[x + row] = only;
            }
            continue;
        }
        std::int64_t t0 = q[x] * c[x], t1 = q[x + 16] * c2, t2 = q[x + 32] * c4, t3 = q[x + 48] * c6;
        std::int64_t t10 = t0 + t2, t11 = t0 - t2, t13 = t1 + t3;
        std::int64_t t12 = (((t1 - t3) * 362) >> 8) - t13;
        t0 = t10 + t13;
        t3 = t10 - t13;
        t1 = t11 + t12;
        t2 = t11 - t12;
        std::int64_t t4 = q[x + 8] * c1, t5 = q[x + 24] * c3, t6 = q[x + 40] * c5, t7 = q[x + 56] * c7;
        const std::int64_t z13 = t6 + t5, z10 = t6 - t5, z11 = t4 + t7, z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = ((z11 - z13) * 362) >> 8;
        const std::int64_t z5 = ((z10 + z12) * 473) >> 8;
        t10 = ((z12 * 277) >> 8) - z5;
        t12 = ((z10 * -669) >> 8) + z5;
        t6 = t12 - t7;
        t5 = t11 - t6;
        t4 = t10 + t5;
        w[x] = t0 + t7;
        w[x + 56] = t0 - t7;
        w[x + 8] = t1 + t6;
        w[x + 48] = t1 - t6;
        w[x + 16] = t2 + t5;
        w[x + 40] = t2 - t5;
        w[x + 32] = t3 + t4;
        w[x + 24] = t3 - t4;
    }
    auto sample = [](std::int64_t value) { return sampleOf[static_cast<std::size_t>((value >> 3) & 1023)]; };
    for (std::size_t row = 0; row < 64; row += 8) {
        const std::int64_t w0 = w[row], w1 = w[row + 1], w2 = w[row + 2], w3 = w[row + 3], w4 = w[row + 4],
                           w5 = w[row + 5], w6 = w[row + 6], w7 = w[row + 7];
        std::int64_t t10 = w0 + w4, t11 = w0 - w4;
        const std::int64_t t13 = w2 + w6;
        std::int64_t t12 = (((w2 - w6) * 362) >> 8) - t13;
        const std::int64_t t0 = t10 + t13, t3 = t10 - t13, t1 = t11 + t12, t2 = t11 - t12;
        const std::int64_t z13 = w5 + w3, z10 = w5 - w3, z11 = w1 + w7, z12 = w1 - w7;
        const std::int64_t t7 = z11 + z13;
        t11 = ((z11 - z13) * 362) >> 8;
        const std::int64_t z5 = ((z10 + z12) * 473) >> 8;
        t10 = ((z12 * 277) >> 8) - z5;
        t12 = ((z10 * -669) >> 8) + z5;
        const std::int64_t t6 = t12 - t7;
        const std::int64_t t5 = t11 - t6;
        const std::int64_t t4 = t10 + t5;
        out[row] = sample(t0 + t7);
        out[row + 1] = sample(t1 + t6);
        out[row + 2] = sample(t2 + t5);
        out[row + 3] = sample(t3 - t4);
        out[row + 4] = sample(t3 + t4);
        out[row + 5] = sample(t2 - t5);
        out[row + 6] = sample(t1 - t6);
        out[row + 7] = sample(t0 - t7);
    }
}

// A value of `size` bits as JPEG writes a signed one: the upper half as it is, the lower below zero.
std::int64_t signedValue(std::uint32_t value, unsigned size) noexcept
{
    return (value >> (size - 1)) ? std::int64_t{value} : std::int64_t{value} + 1 - (std::int64_t{1} << size);
}

// One 8x8 plane of a JPEG block: its samples, and its first coefficient counted on from `before` for the next
// one to count from. False: the frame has a code that stands for nothing.
//
// (How large `before` may grow, which the web's numbers need not care about: by at most 2047 a plane, for at
// most 4 planes of brightness a block and as many blocks as a frame may have, 131,328 at the most; 2.7e8,
// times a quantizer of at most 490, is far within 64 bits, and so is all the transform makes of it.)
bool plane(Bits& bits, const Lookup& dc, const Lookup& ac, std::int64_t& before, const Screen::Quantizers& q,
           Samples& out) noexcept
{
    unsigned entry = dc[bits.peek(16)];
    if (!entry) {
        return false;
    }
    bits.take(static_cast<int>(entry >> 8));
    unsigned size = entry & 255;
    if (size) {
        before += signedValue(bits.take(static_cast<int>(size)), size);
    }
    std::array<std::int64_t, 64> coefficients{};
    bool any = false;
    std::size_t at = 1;
    while (at < 64) {
        entry = ac[bits.peek(16)];
        if (!entry) {
            return false;
        }
        bits.take(static_cast<int>(entry >> 8));
        size = entry & 15;
        if (!size) {
            if ((entry & 0xF0) != format::sixteenZeros) {
                break; // the rest of the block is zero
            }
            at += 16;
            continue;
        }
        at += (entry >> 4) & 15;
        coefficients[format::order[at]] = signedValue(bits.take(static_cast<int>(size)), size);
        any = true;
        ++at;
    }
    if (!any) {
        out.fill(sampleOf[static_cast<std::size_t>(((q[0] * before) >> 3) & 1023)]); // only its average
        return true;
    }
    coefficients[0] = before;
    inverse(coefficients, q, out);
    return true;
}

// The blocks of a screen of `length` pixels, as many as it takes.
int blocksOf(std::int64_t length, int side) noexcept
{
    return static_cast<int>((length + side - 1) / side);
}

} // namespace

Screen::Screen() : m_colours{0x008080, 0xFF8080, 0x808080, 0xC08080} {}

std::size_t Screen::held() const noexcept
{
    return m_picture.rgb.size() + m_kept.size() * sizeof(Kept) + m_rows.size() * sizeof(m_rows.front()) +
           m_changed.size() * sizeof(Rect);
}

namespace {

// What a screen can be, and the strengths there are: a header of anything else is refused (Screen::fit).
bool aScreensSize(const FrameHeader& header) noexcept
{
    return 0 < header.width && header.width <= largest && 0 < header.height && header.height <= largest;
}

bool strengthsThereAre(const FrameHeader& header) noexcept
{
    return 0 <= header.strength && header.strength < 8 && 0 <= header.correctionStrength &&
           header.correctionStrength < 8;
}

} // namespace

bool Screen::resizedBy(const FrameHeader& header) const noexcept
{
    return m_fitted && aScreensSize(header) && strengthsThereAre(header) &&
           (header.width != m_header.width || header.height != m_header.height ||
            header.halfColour != m_header.halfColour);
}

base::Result<void> Screen::fit(const FrameHeader& header)
{
    if (!aScreensSize(header)) {
        return garbled("a screen of " + std::to_string(header.width) + " by " + std::to_string(header.height));
    }
    if (!strengthsThereAre(header)) {
        return garbled("a quantization strength there is none of");
    }
    const int side = header.halfColour ? 16 : 8;
    if (!m_fitted || header.width != m_header.width || header.height != m_header.height ||
        header.halfColour != m_header.halfColour) {
        // A new picture, and the old one let go of: made anew, not resized, so that what is asked for is what
        // this screen needs and no more (a vector that grows asks for half as much again as it had), and what
        // a larger screen before it had is not kept.
        m_picture.width = header.width;
        m_picture.height = header.height;
        m_picture.rgb = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(header.width) *
                                                  static_cast<std::size_t>(header.height));
        // What each block was sent as, for a correction: as though it had been sent as nothing (the web's
        // default for a block it has not kept). Nothing is corrected at half the detail.
        m_keptAcross = blocksOf(header.width, 8);
        // What a frame changes, row by row: as many rows of eight pixels as the screen has, and as many
        // rectangles at the most (made here, so that reading a frame asks for no room).
        m_rows = std::vector<std::pair<int, int>>(static_cast<std::size_t>(blocksOf(header.height, 8)));
        m_changed = std::vector<Rect>(m_rows.size());
        m_changes = 0;
        m_kept = std::vector<Kept>(header.halfColour ? 0
                                                     : static_cast<std::size_t>(m_keptAcross) *
                                                           static_cast<std::size_t>(blocksOf(header.height, 8)),
                                   nothing());
    }
    m_across = std::max(1, blocksOf(std::max(header.gridWidth, 1), side));
    m_down = std::max(1, blocksOf(std::max(header.gridHeight, 1), side));
    if (!m_fitted || header.strength != m_header.strength || header.correctionStrength != m_header.correctionStrength ||
        header.lumaForColour != m_header.lumaForColour) {
        const auto& colour = header.lumaForColour ? lumaQuant : chromaQuant;
        const auto& colourStrengths = header.lumaForColour ? lumaStrengths : chromaStrengths;
        const auto strength = static_cast<std::size_t>(header.strength);
        const auto correction = static_cast<std::size_t>(header.correctionStrength);
        m_quant = {quantizers(lumaQuant, lumaStrengths[strength]), quantizers(colour, colourStrengths[strength]),
                   quantizers(lumaQuant, lumaStrengths[correction]), quantizers(colour, colourStrengths[correction])};
    }
    m_header = header;
    m_fitted = true;
    return {};
}

base::Result<int> Screen::read(const FrameHeader& header, std::span<const std::uint8_t> data)
{
    m_changes = 0;
    if (data.size() > longest) {
        return garbled("a frame longer than a screen can be");
    }
    if (const auto fitted = fit(header); !fitted) {
        return fitted.error();
    }
    const std::size_t pixels = static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height);
    if (data.size() > 2 * (3 * pixels) + slack) {
        return garbled("a frame longer than its screen could take");
    }
    // What it changes is said however it ends from here on.
    std::fill(m_rows.begin(), m_rows.end(), std::pair{m_picture.width, 0});
    struct Collected {
        Screen& screen;
        explicit Collected(Screen& of) : screen(of) {}
        Collected(const Collected&) = delete;
        Collected& operator=(const Collected&) = delete;
        ~Collected() { screen.collect(); }
    } const collected{*this};
    // Blocks it may bring: two for every one its screen has, and a few. Every block is in order by itself, and
    // megabytes of them for a screen of four would be read for minutes.
    const bool half = header.halfColour;
    const int side = half ? 16 : 8;
    const std::int64_t allowed = 2 * std::int64_t{blocksOf(header.width, side)} * blocksOf(header.height, side) + spare;
    Bits bits(data);
    const std::int64_t end =
        32 * (static_cast<std::int64_t>(bits.words()) - 2); // no block begins in the last two words
    const Lookups& tables = lookups();
    std::int64_t dcY = 0;
    std::int64_t dcCb = 0;
    std::int64_t dcCr = 0;
    int tx = 0;
    int ty = 0;
    std::int64_t blocks = 0;
    int changed = 0;
    std::array<Samples, 4> ys{};
    Samples y{};
    Samples cb{};
    Samples cr{};

    // One entry of a colour block's list: which of the four known colours, and perhaps what that one is from now on.
    auto colour = [&bits, this] {
        const std::uint32_t isNew = bits.take(1);
        const std::uint32_t which = bits.take(2);
        if (isNew) {
            m_colours[which] = bits.take(24);
        }
        return static_cast<int>(which);
    };

    while (bits.taken() < end) {
        const unsigned kind = bits.peek(4);
        if (kind == format::endMark) {
            break;
        }
        ++blocks;
        if (blocks > allowed) {
            return garbled("a frame of more blocks than its screen has twice over");
        }
        if (!(blocks & 63) && m_givenUp.load(std::memory_order_relaxed)) {
            return base::Error{"given_up", "a frame nobody waits for any more"};
        }
        const unsigned name = kind & 0x7;
        const bool known = name == format::jpeg || name == format::jpegFine || name == format::correction ||
                           name == format::coloursOne || name == format::coloursTwo || name == format::coloursFour;
        if (!known) {
            // Not a kind of block there is: the sender's own decoder steps three bits on, and to the next block.
            bits.take(3);
        } else {
            bits.take(4);
            if (kind & format::placed) {
                tx = static_cast<int>(bits.take(8));
                ty = static_cast<int>(bits.take(8));
            }
        }
        if (!known) {
            // (nothing of it is drawn)
        } else if (name == format::jpeg || name == format::jpegFine || name == format::correction) {
            const std::size_t first = name != format::jpeg ? 2 : 0;
            const Quantizers& brightness = m_quant[first];
            const Quantizers& colours = m_quant[first + 1];
            if (half && name != format::correction) {
                for (Samples& quarter : ys) {
                    if (!plane(bits, tables.dcLuma, tables.acLuma, dcY, brightness, quarter)) {
                        return garbled("a code that stands for nothing");
                    }
                }
                if (!plane(bits, tables.dcChroma, tables.acChroma, dcCb, colours, cb) ||
                    !plane(bits, tables.dcChroma, tables.acChroma, dcCr, colours, cr)) {
                    return garbled("a code that stands for nothing");
                }
                changed += drawLarge(tx, ty, ys, cb, cr);
            } else {
                if (!plane(bits, tables.dcLuma, tables.acLuma, dcY, brightness, y) ||
                    !plane(bits, tables.dcChroma, tables.acChroma, dcCb, colours, cb) ||
                    !plane(bits, tables.dcChroma, tables.acChroma, dcCr, colours, cr)) {
                    return garbled("a code that stands for nothing");
                }
                if (!half) { // (a correction at half the detail is read, and is not drawn)
                    if (name == format::correction) {
                        // A correction is to what the block was sent as, and the next one is too, not to this one's
                        // result.
                        const Kept& before = was(tx, ty);
                        auto add = [](const Samples& old, Samples& change) {
                            for (std::size_t at = 0; at < 64; ++at) {
                                change[at] = static_cast<std::uint8_t>(std::clamp(old[at] + change[at] - 128, 0, 255));
                            }
                        };
                        add(before.y, y);
                        add(before.cb, cb);
                        add(before.cr, cr);
                    }
                    changed += draw(tx, ty, y, cb, cr, name != format::correction);
                }
            }
        } else {
            const int listed = name == format::coloursOne ? 1 : name == format::coloursTwo ? 2 : 4;
            for (std::size_t one = 0; one < static_cast<std::size_t>(listed); ++one) {
                m_chosen[one] = colour();
            }
            if (listed == 1) {
                const std::uint32_t one = m_colours[static_cast<std::size_t>(m_chosen[0])];
                y.fill(static_cast<std::uint8_t>(one >> 16));
                cb.fill(static_cast<std::uint8_t>(one >> 8));
                cr.fill(static_cast<std::uint8_t>(one));
            } else {
                const int each = listed == 2 ? 1 : 2; // bits a pixel
                for (std::size_t at = 0; at < 64; ++at) {
                    const std::uint32_t picked = m_colours[static_cast<std::size_t>(m_chosen[bits.take(each)])];
                    y[at] = static_cast<std::uint8_t>(picked >> 16);
                    cb[at] = static_cast<std::uint8_t>(picked >> 8);
                    cr[at] = static_cast<std::uint8_t>(picked);
                }
            }
            if (!half) { // (they are not sent with colour at half the detail)
                changed += draw(tx, ty, y, cb, cr, true);
            }
        }
        ++tx;
        if (tx >= m_across) {
            tx = 0;
            ++ty;
            if (ty >= m_down) {
                ty = 0;
            }
        }
    }
    return changed;
}

void Screen::drawn(int top, int high, int left, int wide) noexcept
{
    for (int row = top / 8; row <= (top + high - 1) / 8; ++row) {
        std::pair<int, int>& span = m_rows[static_cast<std::size_t>(row)];
        span.first = std::min(span.first, left);
        span.second = std::max(span.second, left + wide);
    }
}

void Screen::collect() noexcept
{
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        const auto [from, to] = m_rows[row];
        if (to <= from) {
            continue;
        }
        const int top = static_cast<int>(row) * 8;
        const int high = std::min(8, m_picture.height - top);
        Rect* const last = m_changes > 0 ? &m_changed[m_changes - 1] : nullptr;
        if (last != nullptr && last->x == from && last->width == to - from && last->y + last->height == top) {
            last->height += high;
        } else {
            m_changed[m_changes++] = {from, top, to - from, high}; // (one a row at the most)
        }
    }
}

const Screen::Kept& Screen::was(int tx, int ty) const noexcept
{
    // Only a block on the screen was kept; one off it is corrected from nothing (and not drawn).
    if (tx * 8 >= m_picture.width || ty * 8 >= m_picture.height) {
        return nothing();
    }
    return m_kept[static_cast<std::size_t>(ty) * static_cast<std::size_t>(m_keptAcross) + static_cast<std::size_t>(tx)];
}

int Screen::draw(int tx, int ty, const Samples& y, const Samples& cb, const Samples& cr, bool keep)
{
    const int left = tx * 8;
    const int top = ty * 8;
    const int wide = std::min(8, m_picture.width - left);
    const int high = std::min(8, m_picture.height - top);
    if (wide <= 0 || high <= 0) {
        return 0;
    }
    drawn(top, high, left, wide);
    if (keep) {
        Kept& kept = m_kept[static_cast<std::size_t>(ty) * static_cast<std::size_t>(m_keptAcross) +
                            static_cast<std::size_t>(tx)];
        kept.y = y;
        kept.cb = cb;
        kept.cr = cr;
    }
    const std::size_t row = 3 * static_cast<std::size_t>(m_picture.width);
    std::size_t at = 3 * (static_cast<std::size_t>(top) * static_cast<std::size_t>(m_picture.width) +
                          static_cast<std::size_t>(left));
    for (std::size_t j = 0; j < static_cast<std::size_t>(high); ++j) {
        const std::span<std::uint8_t> line = std::span(m_picture.rgb).subspan(at, 3 * static_cast<std::size_t>(wide));
        for (std::size_t i = 0; i < static_cast<std::size_t>(wide); ++i) {
            const std::size_t from = j * 8 + i;
            pixel(line, 3 * i, y[from], cb[from], cr[from]);
        }
        at += row;
    }
    return 1;
}

int Screen::drawLarge(int tx, int ty, const std::array<Samples, 4>& ys, const Samples& cb, const Samples& cr)
{
    const int left = tx * 16;
    const int top = ty * 16;
    const int wide = std::min(16, m_picture.width - left);
    const int high = std::min(16, m_picture.height - top);
    if (wide <= 0 || high <= 0) {
        return 0;
    }
    drawn(top, high, left, wide);
    const std::size_t row = 3 * static_cast<std::size_t>(m_picture.width);
    std::size_t at = 3 * (static_cast<std::size_t>(top) * static_cast<std::size_t>(m_picture.width) +
                          static_cast<std::size_t>(left));
    for (std::size_t j = 0; j < static_cast<std::size_t>(high); ++j) {
        const std::span<std::uint8_t> line = std::span(m_picture.rgb).subspan(at, 3 * static_cast<std::size_t>(wide));
        for (std::size_t i = 0; i < static_cast<std::size_t>(wide); ++i) {
            const std::size_t from = (j >> 1) * 8 + (i >> 1);
            pixel(line, 3 * i, ys[(j >> 3) * 2 + (i >> 3)][(j & 7) * 8 + (i & 7)], cb[from], cr[from]);
        }
        at += row;
    }
    return 1;
}

} // namespace qkvm::bmc::aspeed
