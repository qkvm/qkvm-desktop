#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// A VNC server of the tests' own, as bytes: what an RFB server says in its handshake, and its updates in every
// encoding the reader takes, written from RFC 6143 and TightVNC's description of Tight (as the web's
// tests/fake_vnc.py writes them), with a deflater of its own for ZRLE's and Tight's zlib streams (stored blocks, and
// the fixed codes with copies, each part ended by a flush as zlib's Z_SYNC_FLUSH ends it). Plain C++, so that whatever
// carries the bytes (nothing, a test's WebSocket) can say them. Used by the reader's tests and by core's.
namespace qkvm::rfb::tests {

using Bytes = std::vector<std::uint8_t>;

// Where the pixel at (x, y) of a picture `width` wide begins, in bytes.
inline std::size_t at3(int x, int y, int width)
{
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3;
}

inline void be16(Bytes& into, std::uint32_t value)
{
    into.push_back(static_cast<std::uint8_t>(value >> 8));
    into.push_back(static_cast<std::uint8_t>(value));
}

inline void be32(Bytes& into, std::uint32_t value)
{
    be16(into, value >> 16);
    be16(into, value & 0xFFFF);
}

inline Bytes text(std::string_view said)
{
    return Bytes(said.begin(), said.end());
}

inline Bytes join(std::initializer_list<Bytes> parts)
{
    Bytes joined;
    for (const Bytes& part : parts) {
        joined.insert(joined.end(), part.begin(), part.end());
    }
    return joined;
}

// A picture of the tests' own: three bytes a pixel, row after row.
struct Picture {
    int width = 0;
    int height = 0;
    Bytes rgb;
    std::span<const std::uint8_t> at(int x, int y) const
    {
        return std::span<const std::uint8_t>(rgb).subspan(at3(x, y, width), 3);
    }
};

// Pixels that differ from their neighbours, made of `seed`.
inline Picture pattern(int width, int height, int seed = 0)
{
    Picture made{width, height, Bytes(at3(0, height, width))};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = at3(x, y, width);
            made.rgb[at] = static_cast<std::uint8_t>(x * 7 + seed);
            made.rgb[at + 1] = static_cast<std::uint8_t>(y * 13 + seed * 3);
            made.rgb[at + 2] = static_cast<std::uint8_t>((x + y) * 5 + seed * 7);
        }
    }
    return made;
}

// One colour all over.
inline Picture plain(int width, int height, std::array<std::uint8_t, 3> colour)
{
    Picture made{width, height, {}};
    for (int pixel = 0; pixel < width * height; ++pixel) {
        made.rgb.insert(made.rgb.end(), colour.begin(), colour.end());
    }
    return made;
}

// A picture of four colours, one a quarter: a few subrectangles, a small palette, runs.
inline Picture quarters(int width, int height)
{
    Picture made = plain(width, height, {10, 20, 30});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at = at3(x, y, width);
            made.rgb[at] = static_cast<std::uint8_t>(x < width / 2 ? 10 : 200);
            made.rgb[at + 1] = static_cast<std::uint8_t>(y < height / 2 ? 20 : 150);
        }
    }
    return made;
}

// A picture of two colours, a half each: Tight's palette of two, one bit a pixel, rows padded to a byte.
inline Picture halves(int width, int height)
{
    Picture made = plain(width, height, {10, 20, 30});
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x) {
            made.rgb[at3(x, y, width)] = 200;
        }
    }
    return made;
}

// A part of a picture.
inline Picture part(const Picture& of, int x, int y, int width, int height)
{
    Picture made{width, height, {}};
    for (int row = 0; row < height; ++row) {
        const auto from = of.rgb.begin() + static_cast<std::ptrdiff_t>(at3(x, y + row, of.width));
        made.rgb.insert(made.rgb.end(), from, from + static_cast<std::ptrdiff_t>(at3(width, 0, 0)));
    }
    return made;
}

// --- the handshake ------------------------------------------------------------------------------------------------

inline Bytes greeting(std::string_view version = "RFB 003.008\n")
{
    return text(version);
}

inline Bytes securityTypes(std::initializer_list<std::uint8_t> types)
{
    Bytes said{static_cast<std::uint8_t>(types.size())};
    said.insert(said.end(), types.begin(), types.end());
    return said;
}

inline Bytes word(std::uint32_t value)
{
    Bytes said;
    be32(said, value);
    return said;
}

inline Bytes reason(std::string_view why)
{
    return join({word(static_cast<std::uint32_t>(why.size())), text(why)});
}

inline Bytes serverInit(int width, int height, std::string_view name)
{
    Bytes said;
    be16(said, static_cast<std::uint32_t>(width));
    be16(said, static_cast<std::uint32_t>(height));
    // (The server's own pixel format: the reader asks for its own whatever this says.)
    const std::array<std::uint8_t, 16> format{32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0};
    said.insert(said.end(), format.begin(), format.end());
    be32(said, static_cast<std::uint32_t>(name.size()));
    said.insert(said.end(), name.begin(), name.end());
    return said;
}

// What a client says once it is let in, as the reader says it with these encodings and this format.
inline Bytes firstWords(std::initializer_list<std::int32_t> encodings, bool rgb565 = false)
{
    Bytes said{0, 0, 0, 0};
    const std::array<std::uint8_t, 16> rgb888{32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 0, 8, 16, 0, 0, 0};
    const std::array<std::uint8_t, 16> rgb16{16, 16, 0, 1, 0, 31, 0, 63, 0, 31, 11, 5, 0, 0, 0, 0};
    const auto& format = rgb565 ? rgb16 : rgb888;
    said.insert(said.end(), format.begin(), format.end());
    said.push_back(2);
    said.push_back(0);
    be16(said, static_cast<std::uint32_t>(encodings.size()));
    for (const std::int32_t encoding : encodings) {
        be32(said, static_cast<std::uint32_t>(encoding));
    }
    return said;
}

// --- updates ------------------------------------------------------------------------------------------------------

inline Bytes update(std::initializer_list<Bytes> rects)
{
    Bytes said{0, 0};
    be16(said, static_cast<std::uint32_t>(rects.size()));
    for (const Bytes& rect : rects) {
        said.insert(said.end(), rect.begin(), rect.end());
    }
    return said;
}

inline Bytes head(int x, int y, int width, int height, std::int32_t encoding)
{
    Bytes said;
    for (const int value : {x, y, width, height}) {
        be16(said, static_cast<std::uint32_t>(value));
    }
    be32(said, static_cast<std::uint32_t>(encoding));
    return said;
}

// A pixel as the reader asks for them: R, G, B and a byte of nothing.
inline void pixel888(Bytes& into, std::span<const std::uint8_t> rgb)
{
    into.insert(into.end(), rgb.begin(), rgb.begin() + 3);
    into.push_back(0xEE);
}

inline Bytes rawRect(int x, int y, const Picture& pixels)
{
    Bytes said = head(x, y, pixels.width, pixels.height, 0);
    for (std::size_t at = 0; at < pixels.rgb.size(); at += 3) {
        pixel888(said, std::span<const std::uint8_t>(pixels.rgb).subspan(at, 3));
    }
    return said;
}

// RGB565 as the reader asks for it (little-endian), and what the reader makes of it again.
inline std::uint16_t to565(std::span<const std::uint8_t> rgb)
{
    return static_cast<std::uint16_t>(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}

inline Bytes raw565Rect(int x, int y, const Picture& pixels)
{
    Bytes said = head(x, y, pixels.width, pixels.height, 0);
    for (std::size_t at = 0; at < pixels.rgb.size(); at += 3) {
        const std::uint16_t value = to565(std::span<const std::uint8_t>(pixels.rgb).subspan(at, 3));
        said.push_back(static_cast<std::uint8_t>(value));
        said.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    return said;
}

inline Picture as565(const Picture& pixels)
{
    Picture made{pixels.width, pixels.height, {}};
    for (std::size_t at = 0; at < pixels.rgb.size(); at += 3) {
        const std::uint16_t value = to565(std::span<const std::uint8_t>(pixels.rgb).subspan(at, 3));
        made.rgb.push_back(static_cast<std::uint8_t>(((value >> 11) * 255 + 15) / 31));
        made.rgb.push_back(static_cast<std::uint8_t>((((value >> 5) & 63) * 255 + 31) / 63));
        made.rgb.push_back(static_cast<std::uint8_t>(((value & 31) * 255 + 15) / 31));
    }
    return made;
}

inline Bytes copyRectOf(int x, int y, int width, int height, int fromX, int fromY)
{
    Bytes said = head(x, y, width, height, 1);
    be16(said, static_cast<std::uint32_t>(fromX));
    be16(said, static_cast<std::uint32_t>(fromY));
    return said;
}

inline Bytes desktopSizeRect(int width, int height)
{
    return head(0, 0, width, height, -223);
}

// Hextile: every tile raw (`subrects` false), or a background with each of its pixels that differs from the
// background as a subrectangle of one pixel, coloured.
inline Bytes hextileRect(int x, int y, const Picture& pixels, bool subrects)
{
    Bytes said = head(x, y, pixels.width, pixels.height, 5);
    for (int top = 0; top < pixels.height; top += 16) {
        for (int left = 0; left < pixels.width; left += 16) {
            const int width = std::min(16, pixels.width - left);
            const int height = std::min(16, pixels.height - top);
            if (!subrects) {
                said.push_back(1);
                for (int row = 0; row < height; ++row) {
                    for (int column = 0; column < width; ++column) {
                        pixel888(said, pixels.at(left + column, top + row));
                    }
                }
                continue;
            }
            const std::span<const std::uint8_t> background = pixels.at(left, top);
            Bytes rects;
            int many = 0;
            for (int row = 0; row < height; ++row) {
                for (int column = 0; column < width; ++column) {
                    const std::span<const std::uint8_t> one = pixels.at(left + column, top + row);
                    if (!std::equal(one.begin(), one.end(), background.begin())) {
                        pixel888(rects, one);
                        rects.push_back(static_cast<std::uint8_t>(column << 4 | row));
                        rects.push_back(0);
                        ++many;
                    }
                }
            }
            if (many > 255) {
                said.push_back(1); // (too many to say so: raw)
                for (int row = 0; row < height; ++row) {
                    for (int column = 0; column < width; ++column) {
                        pixel888(said, pixels.at(left + column, top + row));
                    }
                }
                continue;
            }
            said.push_back(static_cast<std::uint8_t>(2 | (many > 0 ? 8 | 16 : 0)));
            pixel888(said, background);
            if (many > 0) {
                said.push_back(static_cast<std::uint8_t>(many));
                said.insert(said.end(), rects.begin(), rects.end());
            }
        }
    }
    return said;
}

// Hextile where every tile is a background and `subrects` subrectangles that each cover the whole tile, coloured: the
// review's attack on finding 2, where 255 subrectangles of 16x16 draw 65,280 pixels from 520 bytes.
inline Bytes hextileAllSubrects(int x, int y, int width, int height, int subrects)
{
    Bytes said = head(x, y, width, height, 5);
    const std::array<std::uint8_t, 3> background{0x10, 0x10, 0x10};
    const std::array<std::uint8_t, 3> colour{0x80, 0x40, 0x20};
    for (int top = 0; top < height; top += 16) {
        for (int left = 0; left < width; left += 16) {
            const int tileWidth = std::min(16, width - left);
            const int tileHeight = std::min(16, height - top);
            said.push_back(static_cast<std::uint8_t>(2 | 8 | 16)); // background, subrects, coloured
            pixel888(said, background);
            said.push_back(static_cast<std::uint8_t>(subrects));
            for (int one = 0; one < subrects; ++one) {
                pixel888(said, colour);
                said.push_back(0); // position: top left
                said.push_back(static_cast<std::uint8_t>((tileWidth - 1) << 4 | (tileHeight - 1)));
            }
        }
    }
    return said;
}

// --- a deflater of the tests' own -----------------------------------------------------------------------------------

// One zlib stream that goes on: each part is its blocks and a flush (an empty stored block, which leaves it at a byte),
// as a server's stream goes on from rectangle to rectangle. Stored blocks, or the fixed codes with copies of what came
// one and three bytes before (a pixel's sample, and the pixel before it); or the fixed codes with copies from as far
// back as deflate reaches (32 KiB), the parts before among it, as a server's zlib copies (`Far`: what a reader that
// forgot the stream's last 32 KiB at a part's end would read wrong, outside review 2.7).
class Deflater {
public:
    enum class Way { Stored, Fixed, Far };
    explicit Deflater(Way way = Way::Fixed) : m_way(way) {}

    Bytes part(std::span<const std::uint8_t> data)
    {
        Bytes said;
        if (!m_begun) {
            said = {0x78, 0x01};
            m_begun = true;
        }
        if (m_way == Way::Stored) {
            for (std::size_t at = 0; at < data.size(); at += 65535) {
                const std::size_t size = std::min<std::size_t>(65535, data.size() - at);
                stored(said, data.subspan(at, size), false);
            }
        } else if (m_way == Way::Far && !data.empty()) {
            farPart(said, data);
        } else if (!data.empty()) {
            bits(said, 0, 1); // (not the last)
            bits(said, 1, 2); // the fixed codes
            for (std::size_t at = 0; at < data.size();) {
                std::size_t length = 0;
                int distance = 0;
                for (const int back : {1, 3}) {
                    std::size_t same = 0;
                    while (at >= static_cast<std::size_t>(back) && at + same < data.size() && same < 258 &&
                           data[at + same] == data[at + same - static_cast<std::size_t>(back)]) {
                        ++same;
                    }
                    if (same >= 3 && same > length) {
                        length = same;
                        distance = back;
                    }
                }
                if (length >= 3) {
                    copy(said, static_cast<int>(length), distance);
                    at += length;
                } else {
                    literal(said, data[at]);
                    ++at;
                }
                m_seen += 1;
            }
            literal(said, 256);
        }
        // The flush: an empty stored block, not the last.
        Bytes nothing;
        stored(said, nothing, false);
        for (const std::uint8_t byte : data) {
            m_adlerA = (m_adlerA + byte) % 65521;
            m_adlerB = (m_adlerB + m_adlerA) % 65521;
        }
        return said;
    }

    // Empty stored blocks, not the last, as many as said: what may follow a part (which ends at a byte) and inflates to
    // nothing, five bytes a block, the cheapest block there is to read.
    static Bytes emptyStoredBlocks(int count)
    {
        Bytes said;
        for (int one = 0; one < count; ++one) {
            said.insert(said.end(), {0x00, 0x00, 0x00, 0xFF, 0xFF});
        }
        return said;
    }

    // Of a `Far` stream: the copies so far that begin in a part before the one they are in, and the farthest back any
    // copy reached.
    int copiesFromPartsBefore() const { return m_crossed; }
    std::size_t farthestCopy() const { return m_farthest; }

    // The stream's end: a last block, and its checksum.
    Bytes end()
    {
        Bytes said;
        bits(said, 1, 1);
        bits(said, 1, 2);
        literal(said, 256);
        flushBits(said);
        be32(said, (m_adlerB << 16) | m_adlerA);
        return said;
    }

    // A whole zlib stream of `count` empty dynamic blocks (the review's attack on finding 1): each is a dynamic block
    // (BTYPE 10) whose only code is end-of-block, about twelve bytes that make the reader build a 32,768-entry table
    // for nothing. Its output is empty, so its Adler-32 is 1. A fresh stream (not this Deflater's running one).
    Bytes emptyDynamicStream(int count)
    {
        Bytes said{0x78, 0x01};
        m_byte = 0;
        m_used = 0;
        for (int one = 0; one < count; ++one) {
            emptyDynamicBlock(said, one == count - 1);
        }
        flushBits(said);
        be32(said, 1); // (the Adler-32 of no bytes)
        return said;
    }

private:
    // A `Far` part: one block of the fixed codes, each place copied from where its next three bytes were last seen (in
    // this part or the ones before, within 32 KiB), or from one or three bytes back, whichever copies more.
    void farPart(Bytes& said, std::span<const std::uint8_t> data)
    {
        constexpr std::size_t reach = 32768;
        // The window: the last 32 KiB of the parts before, then this part; where each place of it is in the stream.
        Bytes window(m_history);
        window.insert(window.end(), data.begin(), data.end());
        const std::size_t start = m_history.size();
        const std::size_t base = m_total - start;
        const auto key = [&window](std::size_t at) {
            return (std::uint32_t{window[at]} << 16) | (std::uint32_t{window[at + 1]} << 8) | window[at + 2];
        };
        const auto note = [&](std::size_t at) {
            if (at + 3 <= window.size()) {
                m_lastAt[key(at)] = base + at;
            }
        };
        for (std::size_t at = 0; at < start; ++at) {
            note(at); // (the parts before, as far as they are kept: noted as they were deflated already)
        }
        bits(said, 0, 1); // (not the last)
        bits(said, 1, 2); // the fixed codes
        for (std::size_t at = start; at < window.size();) {
            std::vector<std::size_t> backs{1, 3};
            if (at + 3 <= window.size()) {
                if (const auto seen = m_lastAt.find(key(at));
                    seen != m_lastAt.end() && seen->second >= base && base + at - seen->second <= reach) {
                    backs.push_back(base + at - seen->second);
                }
            }
            std::size_t length = 0;
            std::size_t distance = 0;
            for (const std::size_t back : backs) {
                std::size_t same = 0;
                while (at >= back && at + same < window.size() && same < 258 &&
                       window[at + same] == window[at + same - back]) {
                    ++same;
                }
                if (same >= 3 && same > length) {
                    length = same;
                    distance = back;
                }
            }
            if (length >= 3) {
                copyFrom(said, static_cast<int>(length), static_cast<int>(distance));
                m_crossed += at - distance < start ? 1 : 0;
                m_farthest = std::max(m_farthest, distance);
                for (std::size_t one = 0; one < length; ++one) {
                    note(at + one);
                }
                at += length;
            } else {
                literal(said, window[at]);
                note(at);
                ++at;
            }
        }
        literal(said, 256);
        m_total += data.size();
        m_history.assign(window.end() - static_cast<std::ptrdiff_t>(std::min(reach, window.size())), window.end());
    }
    // A copy at any distance deflate has (3.2.5): its length, then its distance's symbol (five bits of the fixed code)
    // and extra bits.
    void copyFrom(Bytes& into, int length, int distance)
    {
        static constexpr std::array<int, 30> base{1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                  33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::array<int, 30> extra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        lengthOf(into, length);
        int symbol = 29;
        while (base[static_cast<std::size_t>(symbol)] > distance) {
            --symbol;
        }
        code(into, static_cast<std::uint32_t>(symbol), 5);
        bits(into, static_cast<std::uint32_t>(distance - base[static_cast<std::size_t>(symbol)]),
             extra[static_cast<std::size_t>(symbol)]);
    }
    // One empty dynamic block. Its code-length code gives symbol 1 (a length of 1) one bit, and symbols 0 and 18 two
    // bits each; the main lengths are 256 zeros (symbol 18, repeat), then literal 256 of length 1, then one distance of
    // length 0; the body is the one end-of-block code (a single 0 bit).
    void emptyDynamicBlock(Bytes& into, bool last)
    {
        bits(into, last ? 1 : 0, 1); // BFINAL
        bits(into, 2, 2);            // BTYPE = 2 (dynamic)
        bits(into, 0, 5);            // HLIT: 257 literal/length codes
        bits(into, 0, 5);            // HDIST: 1 distance code
        bits(into, 14, 4);           // HCLEN: 18 code-length codes
        static constexpr std::array<int, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        for (int at = 0; at < 18; ++at) {
            const int sym = order[static_cast<std::size_t>(at)];
            const int len = sym == 1 ? 1 : (sym == 0 || sym == 18) ? 2 : 0;
            bits(into, static_cast<std::uint32_t>(len), 3);
        }
        // The main code lengths, in the code-length code (symbol 18 = "11", 1 = "0", 0 = "10").
        code(into, 0b11, 2);
        bits(into, 138 - 11, 7); // 138 zeros
        code(into, 0b11, 2);
        bits(into, 118 - 11, 7); // and 118 more: literals 0 to 255
        code(into, 0b0, 1);      // literal 256: a length of 1
        code(into, 0b10, 2);     // the one distance: a length of 0
        // The block body: the only literal/length code there is (256, end-of-block), which is one 0 bit.
        code(into, 0b0, 1);
    }
    void bits(Bytes& into, std::uint32_t value, int count)
    {
        for (int bit = 0; bit < count; ++bit) {
            m_byte |= static_cast<std::uint8_t>(((value >> bit) & 1) << m_used);
            if (++m_used == 8) {
                into.push_back(m_byte);
                m_byte = 0;
                m_used = 0;
            }
        }
    }
    // A code, its first bit first.
    void code(Bytes& into, std::uint32_t value, int count)
    {
        for (int bit = count - 1; bit >= 0; --bit) {
            bits(into, (value >> bit) & 1, 1);
        }
    }
    void flushBits(Bytes& into)
    {
        if (m_used > 0) {
            into.push_back(m_byte);
            m_byte = 0;
            m_used = 0;
        }
    }
    void stored(Bytes& into, std::span<const std::uint8_t> data, bool last)
    {
        bits(into, last ? 1 : 0, 1);
        bits(into, 0, 2);
        flushBits(into);
        const auto size = static_cast<std::uint16_t>(data.size());
        into.push_back(static_cast<std::uint8_t>(size));
        into.push_back(static_cast<std::uint8_t>(size >> 8));
        into.push_back(static_cast<std::uint8_t>(~size));
        into.push_back(static_cast<std::uint8_t>(~size >> 8));
        into.insert(into.end(), data.begin(), data.end());
    }
    void literal(Bytes& into, int symbol)
    {
        if (symbol < 144) {
            code(into, static_cast<std::uint32_t>(0x30 + symbol), 8);
        } else if (symbol < 256) {
            code(into, static_cast<std::uint32_t>(0x190 + symbol - 144), 9);
        } else if (symbol < 280) {
            code(into, static_cast<std::uint32_t>(symbol - 256), 7);
        } else {
            code(into, static_cast<std::uint32_t>(0xC0 + symbol - 280), 8);
        }
    }
    // A copy's length: its symbol and extra bits.
    void lengthOf(Bytes& into, int length)
    {
        static constexpr std::array<int, 29> base{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::array<int, 29> extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        int symbol = 28;
        while (base[static_cast<std::size_t>(symbol)] > length) {
            --symbol;
        }
        literal(into, 257 + symbol);
        bits(into, static_cast<std::uint32_t>(length - base[static_cast<std::size_t>(symbol)]),
             extra[static_cast<std::size_t>(symbol)]);
    }
    void copy(Bytes& into, int length, int distance)
    {
        lengthOf(into, length);
        code(into, distance == 1 ? 0U : 2U, 5); // distances 1 and 3: symbols 0 and 2, no extra bits
    }

    Way m_way;
    bool m_begun = false;
    std::uint8_t m_byte = 0;
    int m_used = 0;
    std::size_t m_seen = 0;
    std::uint32_t m_adlerA = 1;
    std::uint32_t m_adlerB = 0;
    // A `Far` stream's: the last 32 KiB deflated before this part, how much in all, where each three bytes were last
    // seen in the stream, and what its copies did.
    Bytes m_history;
    std::size_t m_total = 0;
    std::unordered_map<std::uint32_t, std::size_t> m_lastAt;
    int m_crossed = 0;
    std::size_t m_farthest = 0;
};

// --- ZRLE and Tight ----------------------------------------------------------------------------------------------

enum class ZrleTiles { Raw, Solid, Packed, PlainRuns, PaletteRuns };

// ZRLE's tiles of a picture, each in the one way said (Solid and Packed only where the tile fits it: a tile of one
// colour; at most sixteen colours), before they are deflated.
inline Bytes zrleTiles(const Picture& pixels, ZrleTiles way)
{
    Bytes said;
    for (int top = 0; top < pixels.height; top += 64) {
        for (int left = 0; left < pixels.width; left += 64) {
            const Picture tile =
                part(pixels, left, top, std::min(64, pixels.width - left), std::min(64, pixels.height - top));
            std::vector<std::array<std::uint8_t, 3>> palette;
            std::vector<int> indices;
            for (std::size_t at = 0; at < tile.rgb.size(); at += 3) {
                const std::array<std::uint8_t, 3> one{tile.rgb[at], tile.rgb[at + 1], tile.rgb[at + 2]};
                auto found = std::find(palette.begin(), palette.end(), one);
                if (found == palette.end()) {
                    palette.push_back(one);
                    found = palette.end() - 1;
                }
                indices.push_back(static_cast<int>(found - palette.begin()));
            }
            if (way == ZrleTiles::Raw) {
                said.push_back(0);
                said.insert(said.end(), tile.rgb.begin(), tile.rgb.end());
            } else if (way == ZrleTiles::Solid) {
                said.push_back(1);
                said.insert(said.end(), palette[0].begin(), palette[0].end());
            } else if (way == ZrleTiles::Packed) {
                const auto colours = static_cast<int>(palette.size());
                said.push_back(static_cast<std::uint8_t>(colours));
                for (const auto& colour : palette) {
                    said.insert(said.end(), colour.begin(), colour.end());
                }
                const int bits = colours == 2 ? 1 : colours <= 4 ? 2 : 4;
                for (int row = 0; row < tile.height; ++row) {
                    std::uint8_t byte = 0;
                    int used = 0;
                    for (int column = 0; column < tile.width; ++column) {
                        byte = static_cast<std::uint8_t>(byte | indices[at3(column, row, tile.width) / 3]
                                                                    << (8 - bits - used));
                        used += bits;
                        if (used == 8) {
                            said.push_back(byte);
                            byte = 0;
                            used = 0;
                        }
                    }
                    if (used > 0) {
                        said.push_back(byte);
                    }
                }
            } else {
                const bool ofPalette = way == ZrleTiles::PaletteRuns;
                said.push_back(ofPalette ? static_cast<std::uint8_t>(128 + palette.size()) : 128);
                if (ofPalette) {
                    for (const auto& colour : palette) {
                        said.insert(said.end(), colour.begin(), colour.end());
                    }
                }
                for (std::size_t at = 0; at < indices.size();) {
                    std::size_t run = 1;
                    while (at + run < indices.size() && indices[at + run] == indices[at]) {
                        ++run;
                    }
                    if (ofPalette) {
                        said.push_back(static_cast<std::uint8_t>(indices[at] | (run > 1 ? 0x80 : 0)));
                    } else {
                        const auto& colour = palette[static_cast<std::size_t>(indices[at])];
                        said.insert(said.end(), colour.begin(), colour.end());
                    }
                    if (!ofPalette || run > 1) {
                        std::size_t more = run - 1;
                        while (more >= 255) {
                            said.push_back(255);
                            more -= 255;
                        }
                        said.push_back(static_cast<std::uint8_t>(more));
                    }
                    at += run;
                }
            }
        }
    }
    return said;
}

inline Bytes zrleOf(int x, int y, int width, int height, std::span<const std::uint8_t> deflated)
{
    Bytes said = head(x, y, width, height, 16);
    be32(said, static_cast<std::uint32_t>(deflated.size()));
    said.insert(said.end(), deflated.begin(), deflated.end());
    return said;
}

inline Bytes zrleRect(int x, int y, const Picture& pixels, ZrleTiles way, Deflater& stream)
{
    const Bytes tiles = zrleTiles(pixels, way);
    return zrleOf(x, y, pixels.width, pixels.height, stream.part(tiles));
}

// Tight's length of one to three bytes.
inline void compact(Bytes& into, std::size_t length)
{
    into.push_back(static_cast<std::uint8_t>((length & 0x7F) | (length > 0x7F ? 0x80 : 0)));
    if (length > 0x7F) {
        into.push_back(static_cast<std::uint8_t>(((length >> 7) & 0x7F) | (length > 0x3FFF ? 0x80 : 0)));
        if (length > 0x3FFF) {
            into.push_back(static_cast<std::uint8_t>(length >> 14));
        }
    }
}

inline Bytes tightFill(int x, int y, int width, int height, std::array<std::uint8_t, 3> colour, std::uint8_t resets = 0)
{
    Bytes said = head(x, y, width, height, 7);
    said.push_back(static_cast<std::uint8_t>(0x80 | resets));
    said.insert(said.end(), colour.begin(), colour.end());
    return said;
}

inline Bytes tightJpeg(int x, int y, int width, int height, std::span<const std::uint8_t> jpeg)
{
    Bytes said = head(x, y, width, height, 7);
    said.push_back(0x90);
    compact(said, jpeg.size());
    said.insert(said.end(), jpeg.begin(), jpeg.end());
    return said;
}

enum class TightFilter { Copy, Palette, Gradient };

// A Tight rectangle of a picture, in stream `stream` (0 to 3) with its control byte's `resets`, through the filter
// said (Palette: at most 256 colours); deflated where it is 12 bytes or more, as Tight has it.
inline Bytes tightRect(int x, int y, const Picture& pixels, TightFilter filter, Deflater& deflater, int stream = 0,
                       std::uint8_t resets = 0)
{
    Bytes said = head(x, y, pixels.width, pixels.height, 7);
    said.push_back(static_cast<std::uint8_t>((stream << 4) | resets | (filter == TightFilter::Copy ? 0 : 0x40)));
    Bytes plainBytes;
    if (filter == TightFilter::Palette) {
        std::vector<std::array<std::uint8_t, 3>> palette;
        std::vector<int> indices;
        for (std::size_t at = 0; at < pixels.rgb.size(); at += 3) {
            const std::array<std::uint8_t, 3> one{pixels.rgb[at], pixels.rgb[at + 1], pixels.rgb[at + 2]};
            auto found = std::find(palette.begin(), palette.end(), one);
            if (found == palette.end()) {
                palette.push_back(one);
                found = palette.end() - 1;
            }
            indices.push_back(static_cast<int>(found - palette.begin()));
        }
        said.push_back(1);
        said.push_back(static_cast<std::uint8_t>(palette.size() - 1));
        for (const auto& colour : palette) {
            said.insert(said.end(), colour.begin(), colour.end());
        }
        if (palette.size() <= 2) {
            for (int row = 0; row < pixels.height; ++row) {
                std::uint8_t byte = 0;
                int used = 0;
                for (int column = 0; column < pixels.width; ++column) {
                    byte = static_cast<std::uint8_t>(byte | indices[at3(column, row, pixels.width) / 3] << (7 - used));
                    if (++used == 8) {
                        plainBytes.push_back(byte);
                        byte = 0;
                        used = 0;
                    }
                }
                if (used > 0) {
                    plainBytes.push_back(byte);
                }
            }
        } else {
            for (const int index : indices) {
                plainBytes.push_back(static_cast<std::uint8_t>(index));
            }
        }
    } else if (filter == TightFilter::Gradient) {
        said.push_back(2);
        const std::size_t row = static_cast<std::size_t>(pixels.width) * 3;
        for (int line = 0; line < pixels.height; ++line) {
            for (std::size_t sample = 0; sample < row; ++sample) {
                const std::size_t at = static_cast<std::size_t>(line) * row + sample;
                const int left = sample >= 3 ? pixels.rgb[at - 3] : 0;
                const int up = line > 0 ? pixels.rgb[at - row] : 0;
                const int upLeft = line > 0 && sample >= 3 ? pixels.rgb[at - row - 3] : 0;
                const int guess = std::clamp(left + up - upLeft, 0, 255);
                plainBytes.push_back(static_cast<std::uint8_t>(pixels.rgb[at] - guess));
            }
        }
    } else {
        plainBytes = pixels.rgb;
    }
    if (plainBytes.size() < 12) {
        said.insert(said.end(), plainBytes.begin(), plainBytes.end());
        return said;
    }
    const Bytes deflated = deflater.part(plainBytes);
    compact(said, deflated.size());
    said.insert(said.end(), deflated.begin(), deflated.end());
    return said;
}

// What the reader's tests take for a JPEG: "JPG", its width and height, and its pixels as they are.
inline Bytes madeUpJpeg(const Picture& pixels)
{
    Bytes said = text("JPG");
    be16(said, static_cast<std::uint32_t>(pixels.width));
    be16(said, static_cast<std::uint32_t>(pixels.height));
    said.insert(said.end(), pixels.rgb.begin(), pixels.rgb.end());
    return said;
}

} // namespace qkvm::rfb::tests
