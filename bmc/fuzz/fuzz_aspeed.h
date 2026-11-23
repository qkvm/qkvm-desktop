#pragma once

#include "fuzz_input.h"

#include <qkvm/base/fuzz_promise.h>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

// The harness of ASPEED's frames (fuzz_aspeed.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found): up to
// four frames into one screen, each held to the reader's promises; the address sanitizer looks at every access, and
// libFuzzer's own limits (-malloc_limit_mb, -timeout) at what is asked for and how long it takes.
//
// Its canary (fuzz_promise.h): a frame of other data than the one before it read onto the picture an earlier frame made
// (not made anew: of its size and detail), changing blocks of it, as a console's later frames are read (the seeds'
// frames are each of a size of their own, but the console's screen, sent again the same).
namespace qkvm::fuzz {

inline bool sayable(const bmc::aspeed::FrameHeader& header)
{
    namespace aspeed = bmc::aspeed;
    return 0 < header.width && header.width <= aspeed::largest && 0 < header.height &&
           header.height <= aspeed::largest && 0 <= header.strength && header.strength < 8 &&
           0 <= header.correctionStrength && header.correctionStrength < 8;
}

inline void readAspeed(std::span<const std::uint8_t> input)
{
    namespace aspeed = bmc::aspeed;
    aspeed::Screen screen;
    std::span<const std::uint8_t> rest = input;
    std::span<const std::uint8_t> before; // the frame before's data
    for (int frame = 0; frame < bmc::fuzz::mostFrames && rest.size() >= bmc::fuzz::headerBytes; ++frame) {
        const aspeed::FrameHeader header = bmc::fuzz::headerOf(rest);
        const std::size_t length =
            std::min<std::size_t>(bmc::fuzz::lengthOf(rest.subspan(12, 4)), rest.size() - bmc::fuzz::headerBytes);
        const std::span<const std::uint8_t> body = rest.subspan(bmc::fuzz::headerBytes, length);
        rest = rest.subspan(bmc::fuzz::headerBytes + length);
        const bool other = !std::ranges::equal(body, std::exchange(before, body));
        const bool onto = screen.picture().width > 0 && !screen.resizedBy(header) && other;
        const auto read = screen.read(header, body);
        if (read.ok() && onto && read.value() > 0) {
            reached("a frame of other data read onto the picture an earlier frame made, changing it");
        }
        if (read.ok()) {
            const int side = header.halfColour ? 16 : 8;
            const std::int64_t allowed =
                2 * std::int64_t{(header.width + side - 1) / side} * ((header.height + side - 1) / side) +
                aspeed::spare;
            promise(sayable(header), "a frame of an impossible header was read");
            promise(read.value() >= 0 && read.value() <= allowed, "a frame changed more blocks than it may bring");
        } else {
            promise(read.error().code == "garbled", "a frame was refused for no reason there is");
        }
        const aspeed::Picture& picture = screen.picture();
        promise(picture.width >= 0 && picture.width <= aspeed::largest && picture.height >= 0 &&
                    picture.height <= aspeed::largest &&
                    picture.rgb.size() ==
                        3 * static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height),
                "the picture is not a whole picture");
    }
}

} // namespace qkvm::fuzz
