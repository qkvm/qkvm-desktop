#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <qkvm/base/screen_changes.h>

namespace qkvm::base::screen {

// A card's still of a server's screen (a server's driver of this PC's own wall: its controller's console, read by
// bmc/): the whole screen as one packet of screen_changes.h, which a still's stream takes as it takes a JPEG
// (engine's Codec::Still), made smaller for a card as the web makes its JPEGs of the same screen (kvm.py TIERS,
// tier_for: the smallest of these widths that is at least as wide as the card, never wider than the screen, and past
// the largest the whole screen). Plain C++, as screen_changes.h is.
inline constexpr std::array<int, 3> stillWidths{480, 640, 960};
// The longest still there is: what a still's stream takes of one packet (engine's maxPacketBytes, held to this where
// the engine reads it). A whole screen larger than that is made smaller until it fits.
inline constexpr std::size_t longestStill = std::size_t{8} * 1024 * 1024;

// How wide a still is made of a screen `screenWidth` pixels wide, for a card `asked` pixels wide.
inline int stillWidthFor(int asked, int screenWidth) noexcept
{
    for (const int tier : stillWidths) {
        if (asked <= tier) {
            return tier < screenWidth ? tier : screenWidth;
        }
    }
    return screenWidth;
}

// A screen made smaller: `toWidth` pixels wide, as high as keeps its shape (at least one), each pixel the mean of
// the pixels of the screen that fall in it. Empty where the screen or `rgb` is not one (three bytes a pixel, its rows
// `width * 3` apart), or `toWidth` is none or wider than the screen.
struct Smaller {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};
inline Smaller smaller(int width, int height, std::span<const std::uint8_t> rgb, int toWidth)
{
    if (!detail::aScreen(width, height) || toWidth <= 0 || toWidth > width ||
        rgb.size() != std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return {};
    }
    const auto wide = static_cast<std::int64_t>(width);
    const auto high = static_cast<std::int64_t>(height);
    const std::int64_t toHigh = std::max<std::int64_t>(1, (high * toWidth + wide / 2) / wide);
    Smaller made{toWidth, static_cast<int>(toHigh), {}};
    made.rgb.resize(std::size_t{3} * static_cast<std::size_t>(toWidth) * static_cast<std::size_t>(toHigh));
    // Which pixels of the screen fall in each pixel made: those from its start to the next one's, across and down.
    for (std::int64_t y = 0; y < toHigh; ++y) {
        const std::int64_t top = y * high / toHigh;
        const std::int64_t bottom = std::max(top + 1, (y + 1) * high / toHigh);
        for (std::int64_t x = 0; x < toWidth; ++x) {
            const std::int64_t left = x * wide / toWidth;
            const std::int64_t right = std::max(left + 1, (x + 1) * wide / toWidth);
            std::array<std::uint64_t, 3> sum{};
            for (std::int64_t from = top; from < bottom; ++from) {
                const std::size_t row = static_cast<std::size_t>(from * wide * 3);
                for (std::int64_t across = left; across < right; ++across) {
                    const std::size_t at = row + static_cast<std::size_t>(across * 3);
                    sum[0] += rgb[at];
                    sum[1] += rgb[at + 1];
                    sum[2] += rgb[at + 2];
                }
            }
            const auto count = static_cast<std::uint64_t>((bottom - top) * (right - left));
            const std::size_t to = static_cast<std::size_t>((y * toWidth + x) * 3);
            for (std::size_t sign = 0; sign < 3; ++sign) {
                made.rgb[to + sign] = static_cast<std::uint8_t>((sum[sign] + count / 2) / count);
            }
        }
    }
    return made;
}

// The still of a screen for a card `asked` pixels wide: one packet of the whole of it (screen_changes.h), as wide as
// stillWidthFor says, and no longer than longestStill. Empty where the screen or `rgb` is not one.
template <typename Bytes = std::vector<std::byte>>
Bytes still(int width, int height, std::span<const std::uint8_t> rgb, int asked)
{
    if (!detail::aScreen(width, height) ||
        rgb.size() != std::size_t{3} * static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return {};
    }
    int toWidth = stillWidthFor(asked, width);
    // (Narrower, a pixel at a time, until the whole of it fits: the height follows the width.)
    const auto bytesAt = [width, height](int wide) {
        const auto wideBy = static_cast<std::size_t>(wide);
        const std::size_t high =
            std::max<std::size_t>(1, (static_cast<std::size_t>(height) * wideBy + static_cast<std::size_t>(width) / 2) /
                                         static_cast<std::size_t>(width));
        return headBytes + rectBytes + std::size_t{3} * wideBy * high;
    };
    while (toWidth > 1 && bytesAt(toWidth) > longestStill) {
        --toWidth;
    }
    if (toWidth == width) {
        return whole<Bytes>(width, height, rgb);
    }
    const Smaller made = smaller(width, height, rgb, toWidth);
    return whole<Bytes>(made.width, made.height, made.rgb);
}

} // namespace qkvm::base::screen
