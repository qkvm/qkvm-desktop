#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

// What changed of a screen, as one packet: the size of the screen, and the rectangles of it that changed, each with
// its pixels. A server's screen goes so from its controller's console (core's ServerScreen, which bmc/ reads) to the
// engine (Codec::Rgb), which copies to the graphics card what changed and nothing else: a screen that stands still
// costs nothing, and a clock that ticks costs its rectangle. A packet of one rectangle as large as the screen is the
// whole screen: what the engine begins with, and a card's still (Codec::Still takes it as it takes a JPEG).
//
// Written by core, read by the engine, which know nothing of each other: so it is here, plain C++, in one place.
// The bytes, every number of 16 bits, least significant byte first:
//
//   "QKSC"                             what it is (a JPEG and a PNG begin otherwise)
//   width, height                      of the screen: 1 to `widest` by 1 to `tallest`
//   count, 0                           how many rectangles: 1 to `mostRects`; and a number that is nought
//   x, y, width, height                each rectangle: inside the screen, and none empty
//   the pixels                         of each rectangle in turn, its rows from the top, three bytes a pixel:
//                                      red, green, blue
//
// Nothing else, and nothing after. A reader takes no packet that is not so to the byte.
namespace qkvm::base::screen {

inline constexpr std::array<std::uint8_t, 4> mark{'Q', 'K', 'S', 'C'};
// The largest screen there is room for (the engine's largest picture), and the most rectangles one packet holds: a
// frame of ASPEED's changes rows of eight pixels, one rectangle a row at the most (bmc's aspeed.h), and its screens
// are 2048 pixels high at the most.
inline constexpr int widest = 4096;
inline constexpr int tallest = 2304;
inline constexpr int mostRects = 4096;
inline constexpr std::size_t headBytes = 12;
inline constexpr std::size_t rectBytes = 8;
// The longest packet there is: every rectangle's head, and a screen of the largest size whole.
inline constexpr std::size_t longest = headBytes + rectBytes * mostRects + std::size_t{3} * widest * tallest;

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const Rect&) const = default;
};

// A packet, read: the screen's size, the rectangles, and the pixels of all of them one after the other (within the
// packet that was read).
struct Changes {
    int width = 0;
    int height = 0;
    std::vector<Rect> rects;
    std::span<const std::byte> pixels;

    // One rectangle, and it is the whole screen.
    bool whole() const noexcept { return rects.size() == 1 && rects.front() == Rect{0, 0, width, height}; }
};

namespace detail {

inline int read16(std::span<const std::byte> bytes, std::size_t at) noexcept
{
    return static_cast<int>(static_cast<unsigned>(bytes[at]) | (static_cast<unsigned>(bytes[at + 1]) << 8));
}

inline bool fits(const Rect& rect, int width, int height) noexcept
{
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 && rect.x <= width - rect.width &&
           rect.y <= height - rect.height;
}

inline bool aScreen(int width, int height) noexcept
{
    return width > 0 && width <= widest && height > 0 && height <= tallest;
}

} // namespace detail

// The head of a packet alone: the screen's size, and whether it is the whole screen. Nothing where the head is not
// one (the rectangles and their pixels are not looked at: read() is what takes a packet).
struct Head {
    int width = 0;
    int height = 0;
    bool whole = false;
};
inline std::optional<Head> headOf(std::span<const std::byte> packet) noexcept
{
    if (packet.size() < headBytes + rectBytes || std::memcmp(packet.data(), mark.data(), mark.size()) != 0) {
        return std::nullopt;
    }
    const Head head{detail::read16(packet, 4), detail::read16(packet, 6),
                    detail::read16(packet, 8) == 1 && detail::read16(packet, headBytes) == 0 &&
                        detail::read16(packet, headBytes + 2) == 0 &&
                        detail::read16(packet, headBytes + 4) == detail::read16(packet, 4) &&
                        detail::read16(packet, headBytes + 6) == detail::read16(packet, 6)};
    if (!detail::aScreen(head.width, head.height)) {
        return std::nullopt;
    }
    return head;
}

// A packet, read; nothing where it is not one, to the byte.
inline std::optional<Changes> read(std::span<const std::byte> packet)
{
    if (packet.size() < headBytes || packet.size() > longest ||
        std::memcmp(packet.data(), mark.data(), mark.size()) != 0) {
        return std::nullopt;
    }
    Changes changes;
    changes.width = detail::read16(packet, 4);
    changes.height = detail::read16(packet, 6);
    const int count = detail::read16(packet, 8);
    if (!detail::aScreen(changes.width, changes.height) || count < 1 || count > mostRects ||
        detail::read16(packet, 10) != 0 || packet.size() < headBytes + rectBytes * static_cast<std::size_t>(count)) {
        return std::nullopt;
    }
    changes.rects.reserve(static_cast<std::size_t>(count));
    std::size_t pixels = 0;
    for (int one = 0; one < count; ++one) {
        const std::size_t at = headBytes + rectBytes * static_cast<std::size_t>(one);
        const Rect rect{detail::read16(packet, at), detail::read16(packet, at + 2), detail::read16(packet, at + 4),
                        detail::read16(packet, at + 6)};
        if (!detail::fits(rect, changes.width, changes.height)) {
            return std::nullopt;
        }
        pixels += std::size_t{3} * static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height);
        changes.rects.push_back(rect);
    }
    const std::size_t from = headBytes + rectBytes * static_cast<std::size_t>(count);
    if (packet.size() - from != pixels) {
        return std::nullopt;
    }
    changes.pixels = packet.subspan(from);
    return changes;
}

// A packet of these rectangles of a screen: `rgb` is the whole screen, three bytes a pixel, its rows `width * 3`
// apart. Empty where the screen, `rgb` or a rectangle is not as read() takes them. Written into bytes of the
// caller's kind: anything that is resized and holds its bytes one after the other (a QByteArray, say, in which the
// packet is then handed on as it is, megabytes not copied again).
template <typename Bytes = std::vector<std::byte>>
Bytes write(int width, int height, std::span<const std::uint8_t> rgb, std::span<const Rect> rects)
{
    if (!detail::aScreen(width, height) || rects.empty() || rects.size() > static_cast<std::size_t>(mostRects) ||
        rgb.size() != std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return {};
    }
    std::size_t pixels = 0;
    for (const Rect& rect : rects) {
        if (!detail::fits(rect, width, height)) {
            return {};
        }
        pixels += std::size_t{3} * static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height);
    }
    Bytes packet;
    packet.resize(static_cast<decltype(packet.size())>(headBytes + rectBytes * rects.size() + pixels));
    auto* to = reinterpret_cast<std::byte*>(packet.data());
    const auto put16 = [&to](int value) {
        *to++ = static_cast<std::byte>(value & 0xFF);
        *to++ = static_cast<std::byte>((value >> 8) & 0xFF);
    };
    std::memcpy(to, mark.data(), mark.size());
    to += mark.size();
    put16(width);
    put16(height);
    put16(static_cast<int>(rects.size()));
    put16(0);
    for (const Rect& rect : rects) {
        put16(rect.x);
        put16(rect.y);
        put16(rect.width);
        put16(rect.height);
    }
    const auto row = std::size_t{3} * static_cast<std::size_t>(width);
    for (const Rect& rect : rects) {
        const auto across = std::size_t{3} * static_cast<std::size_t>(rect.width);
        for (int y = rect.y; y < rect.y + rect.height; ++y) {
            const std::span<const std::uint8_t> line = rgb.subspan(
                static_cast<std::size_t>(y) * row + std::size_t{3} * static_cast<std::size_t>(rect.x), across);
            std::memcpy(to, line.data(), line.size());
            to += line.size();
        }
    }
    return packet;
}

// The whole screen, as one packet.
template <typename Bytes = std::vector<std::byte>>
Bytes whole(int width, int height, std::span<const std::uint8_t> rgb)
{
    const Rect all{0, 0, width, height};
    return write<Bytes>(width, height, rgb, std::span(&all, 1));
}

} // namespace qkvm::base::screen
