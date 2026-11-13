#pragma once

#include <qkvm/bmc/aspeed.h>
#include <qkvm/bmc/aspeed_format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

// Frames in ASPEED's video format, made for the tests: what a management controller's chip would send.
//
// The other half of qkvm/bmc/aspeed.h, written as the web's app/tests/aspeed_frames.py writes them (from
// the same description of the format, and with the same tables). A frame is built block by block and comes
// out as the bytes a controller would send, with the header that goes with them. Plain C++, so that the
// fuzz targets make their first inputs with it too.
namespace qkvm::bmc::aspeed::frames {

using Coefficients = std::array<int, 64>; // the 64 quantized coefficients of one 8x8 plane, row after row
using Place = std::optional<std::pair<int, int>>;

// One entry of a colour block's list: which of the four known colours, and, if it is to be a new colour
// from now on, that colour (Y, Cb, Cr in one number).
struct Listed {
    int which = 0;
    std::optional<std::uint32_t> colour;
};

struct Options {
    bool halfColour = false;
    int strength = 4;
    int correctionStrength = 7;
    bool lumaForColour = false;
};

// Numbers that look like chance and are the same on every computer, as the web's tests make them.
class Numbers {
public:
    explicit Numbers(std::uint64_t seed) : m_state(seed) {}
    int operator()(int limit)
    {
        m_state = (m_state * 1103515245 + 12345) & 0x7FFFFFFF;
        return static_cast<int>((m_state >> 8) % static_cast<std::uint64_t>(limit));
    }

private:
    std::uint64_t m_state;
};

[[noreturn]] inline void cannotWrite(const char* what)
{
    std::fprintf(stderr, "aspeed_frames: %s\n", what);
    std::abort();
}

// What each value of a table is written as: its code and the code's length in bits (0: it has none).
struct Code {
    std::uint32_t code = 0;
    int length = 0;
};

inline std::array<Code, 256> codes(const format::Huffman& table)
{
    std::array<Code, 256> found{};
    std::uint32_t code = 0;
    std::size_t at = 0;
    for (int length = 1; length <= 16; ++length) {
        for (int one = 0; one < table.counts[static_cast<std::size_t>(length - 1)]; ++one) {
            found[table.values[at]] = {code, length};
            ++code;
            ++at;
        }
        code <<= 1;
    }
    return found;
}

inline int bitLength(int value)
{
    int length = 0;
    for (unsigned rest = static_cast<unsigned>(value < 0 ? -value : value); rest; rest >>= 1) {
        ++length;
    }
    return length;
}

// One frame being written. Blocks go in the order they are added; `data` is the finished frame.
class Frame {
public:
    Frame(int width, int height, Options options = {})
    {
        const int side = options.halfColour ? 16 : 8;
        m_header = {width,
                    height,
                    (width + side - 1) / side * side,
                    (height + side - 1) / side * side,
                    options.halfColour,
                    options.strength,
                    options.correctionStrength,
                    options.lumaForColour};
    }

    const FrameHeader& header() const { return m_header; }

    // A JPEG block: its planes' coefficients.
    void jpeg(const Coefficients& y, const Coefficients& cb, const Coefficients& cr, Place at = {}, bool fine = false,
              bool correction = false)
    {
        start(correction ? format::correction : fine ? format::jpegFine : format::jpeg, at);
        plane(y, 0);
        plane(cb, 1);
        plane(cr, 2);
    }

    // The same with colour at half the detail: four planes of brightness, one a quarter.
    void jpeg(const std::array<Coefficients, 4>& ys, const Coefficients& cb, const Coefficients& cr, Place at = {},
              bool fine = false, bool correction = false)
    {
        start(correction ? format::correction : fine ? format::jpegFine : format::jpeg, at);
        for (const Coefficients& y : ys) {
            plane(y, 0);
        }
        plane(cb, 1);
        plane(cr, 2);
    }

    // A block of one, two or four colours: for each, which of the four known ones it is and, if it is to be a
    // new colour from now on, that colour; then which one every pixel is.
    void colours(const std::vector<Listed>& listed, const std::vector<int>& pixels = {}, Place at = {})
    {
        const std::size_t many = listed.size();
        if (many != 1 && many != 2 && many != 4) {
            cannotWrite("a colour block lists one, two or four colours");
        }
        start(many == 1 ? format::coloursOne : many == 2 ? format::coloursTwo : format::coloursFour, at);
        for (const Listed& one : listed) {
            put(one.colour ? 1 : 0, 1);
            put(one.which, 2);
            if (one.colour) {
                put(*one.colour, 24);
            }
        }
        if (many > 1) {
            if (pixels.size() != 64) {
                cannotWrite("a block of more than one colour says which every pixel is");
            }
            for (const int pixel : pixels) {
                put(pixel, many == 2 ? 1 : 2);
            }
        }
    }

    // A block of no kind there is (its first three bits; the sender's decoder takes no more of it).
    void odd(unsigned kind) { put(kind >> 1, 3); }

    // The finished frame: its bits, the end mark, nothing to the end of the word, and three words of nothing
    // more (no block begins in a frame's last two words).
    std::vector<std::uint8_t> data() const
    {
        Frame ended = *this;
        ended.put(format::endMark, 4);
        std::vector<std::uint32_t> words = ended.m_words;
        words.insert(words.end(), 3, 0);
        std::vector<std::uint8_t> found;
        found.reserve(4 * words.size());
        for (const std::uint32_t word : words) {
            for (int shift = 0; shift < 32; shift += 8) {
                found.push_back(static_cast<std::uint8_t>(word >> shift));
            }
        }
        return found;
    }

    // Bits put so far, for whoever cuts a frame at a block's edge.
    std::size_t bits() const { return m_length; }

    // The `length` lowest bits of `value` (up to 32; a value below zero as two's complement has it).
    void put(std::int64_t value, int length)
    {
        const auto bits = static_cast<std::uint64_t>(value);
        while (length > 0) {
            const auto used = static_cast<int>(m_length % 32);
            if (!used) {
                m_words.push_back(0);
            }
            const int now = std::min(32 - used, length);
            const std::uint64_t part = (bits >> (length - now)) & ((std::uint64_t{1} << now) - 1);
            m_words.back() |= static_cast<std::uint32_t>(part << (32 - used - now));
            length -= now;
            m_length += static_cast<std::size_t>(now);
        }
    }

private:
    void start(unsigned kind, Place at)
    {
        if (!at) {
            put(kind, 4);
            return;
        }
        put(kind | format::placed, 4);
        put(at->first, 8);
        put(at->second, 8);
    }

    void plane(const Coefficients& coefficients, int which)
    {
        static const std::array<Code, 256> dcLuma = codes(format::dcLuma);
        static const std::array<Code, 256> dcChroma = codes(format::dcChroma);
        static const std::array<Code, 256> acLuma = codes(format::acLuma);
        static const std::array<Code, 256> acChroma = codes(format::acChroma);
        const std::array<Code, 256>& dc = which == 0 ? dcLuma : dcChroma;
        const std::array<Code, 256>& ac = which == 0 ? acLuma : acChroma;
        auto code = [this](const std::array<Code, 256>& table, int value) {
            if (value < 0 || value > 255 || !table[static_cast<std::size_t>(value)].length) {
                cannotWrite("a value the format has no code for");
            }
            put(table[static_cast<std::size_t>(value)].code, table[static_cast<std::size_t>(value)].length);
        };
        const auto planeIs = static_cast<std::size_t>(which);
        const int difference = coefficients[0] - m_before[planeIs];
        m_before[planeIs] = coefficients[0];
        const int size = bitLength(difference);
        code(dc, size);
        if (size) {
            put(difference > 0 ? difference : difference + (1 << size) - 1, size);
        }
        int last = 0;
        for (int k = 1; k < 64; ++k) {
            if (coefficients[format::order[static_cast<std::size_t>(k)]]) {
                last = k;
            }
        }
        int run = 0;
        for (int k = 1; k <= last; ++k) {
            const int value = coefficients[format::order[static_cast<std::size_t>(k)]];
            if (!value) {
                ++run;
                continue;
            }
            while (run > 15) {
                code(ac, format::sixteenZeros);
                run -= 16;
            }
            const int length = bitLength(value);
            code(ac, (run << 4) | length);
            put(value > 0 ? value : value + (1 << length) - 1, length);
            run = 0;
        }
        if (last < 63) {
            code(ac, format::restIsZero);
        }
    }

    FrameHeader m_header;
    std::vector<std::uint32_t> m_words;
    std::size_t m_length = 0;
    std::array<int, 3> m_before{}; // each plane's first coefficient counts on from the last one's
};

// A screen that looks like a server's text console from across the room: a title bar, and lines of "text".
inline Frame consoleScreen(int width = 800, int height = 600, int lines = 14, int seed = 1)
{
    Numbers below(static_cast<std::uint64_t>(seed));
    constexpr std::uint32_t dark = 0x108080; // nearly black
    constexpr std::uint32_t bar = 0x5A9A70;  // a muted blue
    constexpr std::uint32_t ink = 0xD08080;  // light grey
    Frame frame(width, height);
    const int across = (width + 7) / 8;
    const int down = (height + 7) / 8;
    std::vector<int> lengths(static_cast<std::size_t>(down) + 2 * static_cast<std::size_t>(lines) + 2, -1);
    for (int line = 0; line < lines; ++line) {
        lengths[2 + 2 * static_cast<std::size_t>(line)] = 6 + below(across - 12);
    }
    for (int y = 0; y < down; ++y) {
        for (int x = 0; x < across; ++x) {
            const int length = lengths[static_cast<std::size_t>(y)];
            if (y == 0) {
                frame.colours({{1, bar}});
            } else if (length >= 0 && 1 <= x && x <= length && below(6)) {
                // A letter, more or less: a few strokes in a box of 8 by 8, light on dark.
                std::array<int, 5> strokes{};
                for (int& stroke : strokes) {
                    const int one = below(256);
                    stroke = one & below(256);
                }
                std::vector<int> pixels;
                for (int row = 0; row < 8; ++row) {
                    for (int col = 0; col < 8; ++col) {
                        const bool inked = 1 <= row && row <= 6 && col < 6;
                        pixels.push_back(inked ? (strokes[static_cast<std::size_t>(std::min(row - 1, 4))] >> col) & 1
                                               : 0);
                    }
                }
                frame.colours({{0, dark}, {2, ink}}, pixels);
            } else {
                frame.colours({{0, dark}});
            }
        }
    }
    return frame;
}

} // namespace qkvm::bmc::aspeed::frames
