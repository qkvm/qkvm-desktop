#pragma once

#include "../tests/aspeed_frames.h"
#include "fuzz_input.h"

#include <qkvm/bmc/ami_console.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// The first inputs of bmc's fuzz targets, for the seed maker (core/fuzz/make_seeds.cpp): frames the tests' own writer
// makes, of every kind of block, a few to an input, as fuzz_input.h lays them out (fuzz_aspeed.h); and what a
// controller's console sends (its first words, and those frames in pieces), as fuzz_ami_console.h takes it.
namespace qkvm::bmc::fuzz {

namespace seeds {

inline aspeed::frames::Coefficients planeOf(aspeed::frames::Numbers& below, int busy)
{
    aspeed::frames::Coefficients found{};
    found[0] = below(301) - 150;
    for (std::size_t at = 1; at < 64; ++at) {
        if (below(100) < busy) {
            found[at] = below(121) - 60;
        }
    }
    return found;
}

inline aspeed::frames::Frame someFrame(aspeed::frames::Numbers& below)
{
    using aspeed::frames::Coefficients;
    const int width = 1 + below(160);
    const int height = 1 + below(120);
    const bool half = below(4) == 0;
    const int strength = below(8);
    const int correction = below(8);
    const bool luma = below(2) == 0;
    aspeed::frames::Frame frame(
        width, height,
        {.halfColour = half, .strength = strength, .correctionStrength = correction, .lumaForColour = luma});
    const int blocks = 1 + below(60);
    for (int block = 0; block < blocks; ++block) {
        aspeed::frames::Place at;
        if (below(3) == 0) {
            const int x = below(24);
            at = std::pair{x, below(18)};
        }
        const int kind = below(6);
        const int busy = below(30);
        if (kind == 0 && half) {
            std::array<Coefficients, 4> ys{};
            for (Coefficients& y : ys) {
                y = planeOf(below, busy);
            }
            const Coefficients cb = planeOf(below, busy);
            const Coefficients cr = planeOf(below, busy);
            frame.jpeg(ys, cb, cr, at, below(2) == 0);
        } else if (kind <= 1) {
            const Coefficients y = planeOf(below, busy);
            const Coefficients cb = planeOf(below, busy);
            const Coefficients cr = planeOf(below, busy);
            frame.jpeg(y, cb, cr, at, below(3) == 0, kind == 1);
        } else {
            const std::size_t listed = kind == 2 ? 1 : kind == 3 ? 2 : 4;
            std::vector<aspeed::frames::Listed> colours;
            for (std::size_t one = 0; one < listed; ++one) {
                aspeed::frames::Listed entry{below(4), std::nullopt};
                if (below(2) == 0) {
                    entry.colour = static_cast<std::uint32_t>(below(1 << 24));
                }
                colours.push_back(entry);
            }
            std::vector<int> pixels;
            for (int pixel = 0; listed > 1 && pixel < 64; ++pixel) {
                pixels.push_back(below(static_cast<int>(listed)));
            }
            frame.colours(colours, pixels, at);
        }
    }
    return frame;
}

// The facts a frame's first piece begins with, as a controller writes them (ami_console.h, frameFacts).
inline std::vector<std::uint8_t> factsOf(const aspeed::FrameHeader& header, std::size_t length)
{
    std::vector<std::uint8_t> found(ami::frameFacts);
    auto put16 = [&found](std::size_t at, int value) {
        found[at] = static_cast<std::uint8_t>(value);
        found[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    put16(4, header.width);
    put16(6, header.height);
    put16(13, header.gridWidth);
    put16(15, header.gridHeight);
    found[44] = static_cast<std::uint8_t>(header.strength);
    found[45] = header.lumaForColour ? 1 : 0;
    found[47] = static_cast<std::uint8_t>(header.correctionStrength);
    found[55] = header.halfColour ? 1 : 0;
    found[69] = static_cast<std::uint8_t>(length);
    found[70] = static_cast<std::uint8_t>(length >> 8);
    found[71] = static_cast<std::uint8_t>(length >> 16);
    return found;
}

} // namespace seeds

// Forty-eight inputs of up to four frames each (the first a console's screen as the tests draw it).
inline std::vector<std::vector<std::uint8_t>> aspeedSeeds()
{
    std::vector<std::vector<std::uint8_t>> made;
    for (int seed = 0; seed < 48; ++seed) {
        aspeed::frames::Numbers below(static_cast<std::uint64_t>(seed) + 1);
        std::vector<std::uint8_t> input;
        const int frames = 1 + below(mostFrames);
        for (int one = 0; one < frames; ++one) {
            const aspeed::frames::Frame frame =
                seed == 0 ? aspeed::frames::consoleScreen(160, 120, 6, 3) : seeds::someFrame(below);
            putFrame(input, frame.header(), frame.data());
        }
        made.push_back(std::move(input));
    }
    return made;
}

// Twenty-four streams of a console: its first words, then frames in pieces, each behind a step of up to 4096 bytes.
inline std::vector<std::vector<std::uint8_t>> consoleSeeds()
{
    std::vector<std::vector<std::uint8_t>> made;
    for (int seed = 0; seed < 24; ++seed) {
        aspeed::frames::Numbers below(static_cast<std::uint64_t>(seed) + 101);
        const int step = 1 + below(4096);
        std::vector<std::uint8_t> input = {static_cast<std::uint8_t>(step), static_cast<std::uint8_t>(step >> 8)};
        auto put = [&input](const ami::Bytes& more) { input.insert(input.end(), more.begin(), more.end()); };
        put(ami::message(ami::kind::allowed));
        put(ami::message(ami::kind::passSeen, static_cast<std::uint16_t>(below(3)), ami::Bytes{1}));
        for (int one = 1 + below(3); one > 0; --one) {
            const aspeed::frames::Frame frame = seeds::someFrame(below);
            const ami::Bytes data = frame.data();
            ami::Bytes first = {0, 0};
            const ami::Bytes facts = seeds::factsOf(frame.header(), data.size());
            first.insert(first.end(), facts.begin(), facts.end());
            std::size_t at = std::min(data.size(), static_cast<std::size_t>(below(2000)));
            first.insert(first.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(at));
            put(ami::message(ami::kind::video, 0, first));
            while (at < data.size()) {
                const std::size_t to = std::min(data.size(), at + 1 + static_cast<std::size_t>(below(3000)));
                ami::Bytes piece = {0, 0};
                piece.insert(piece.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                             data.begin() + static_cast<std::ptrdiff_t>(to));
                put(ami::message(ami::kind::video, 0, piece));
                at = to;
            }
            put(ami::message(ami::kind::stillThere));
        }
        made.push_back(std::move(input));
    }
    return made;
}

} // namespace qkvm::bmc::fuzz
