#include "../tests/aspeed_frames.h"
#include "fuzz_input.h"

#include <qkvm/bmc/ami_console.h>

#include <cstdio>
#include <fstream>
#include <string>

// The first inputs of the fuzz targets: frames the tests' own writer makes, of every kind of block, a few to an
// input, as fuzz_input.h lays them out, into the first folder named, one file each; and into the second, what a
// controller's console sends (its first words, and those frames in pieces), as fuzz_ami_console.cpp takes it.
//
//     fuzz_bmc_seeds <folder> [<console folder>]
using namespace qkvm::bmc;
using aspeed::frames::Coefficients;
using aspeed::frames::Frame;

namespace {

Coefficients planeOf(aspeed::frames::Numbers& below, int busy)
{
    Coefficients found{};
    found[0] = below(301) - 150;
    for (std::size_t at = 1; at < 64; ++at) {
        if (below(100) < busy) {
            found[at] = below(121) - 60;
        }
    }
    return found;
}

Frame someFrame(aspeed::frames::Numbers& below)
{
    const int width = 1 + below(160);
    const int height = 1 + below(120);
    const bool half = below(4) == 0;
    const int strength = below(8);
    const int correction = below(8);
    const bool luma = below(2) == 0;
    Frame frame(width, height,
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
std::vector<std::uint8_t> factsOf(const aspeed::FrameHeader& header, std::size_t length)
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

bool consoleSeeds(const std::string& folder)
{
    for (int seed = 0; seed < 24; ++seed) {
        aspeed::frames::Numbers below(static_cast<std::uint64_t>(seed) + 101);
        const int step = 1 + below(4096);
        std::vector<std::uint8_t> input = {static_cast<std::uint8_t>(step), static_cast<std::uint8_t>(step >> 8)};
        auto put = [&input](const ami::Bytes& more) { input.insert(input.end(), more.begin(), more.end()); };
        put(ami::message(ami::kind::allowed));
        put(ami::message(ami::kind::passSeen, static_cast<std::uint16_t>(below(3)), ami::Bytes{1}));
        for (int one = 1 + below(3); one > 0; --one) {
            const Frame frame = someFrame(below);
            const ami::Bytes data = frame.data();
            ami::Bytes first = {0, 0};
            const ami::Bytes facts = factsOf(frame.header(), data.size());
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
        std::ofstream out(folder + "/console-" + std::to_string(seed), std::ios::binary);
        out.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
        if (!out) {
            return false;
        }
    }
    return true;
}

int made(int argc, char** argv)
{
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "fuzz_bmc_seeds <folder> [<console folder>]\n");
        return 2;
    }
    const std::string folder = argv[1];
    for (int seed = 0; seed < 48; ++seed) {
        aspeed::frames::Numbers below(static_cast<std::uint64_t>(seed) + 1);
        std::vector<std::uint8_t> input;
        const int frames = 1 + below(fuzz::mostFrames);
        for (int one = 0; one < frames; ++one) {
            const Frame frame = seed == 0 ? aspeed::frames::consoleScreen(160, 120, 6, 3) : someFrame(below);
            fuzz::putFrame(input, frame.header(), frame.data());
        }
        std::ofstream out(folder + "/seed-" + std::to_string(seed), std::ios::binary);
        out.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
        if (!out) {
            std::fprintf(stderr, "could not write into %s\n", folder.c_str());
            return 1;
        }
    }
    if (argc == 3 && !consoleSeeds(argv[2])) {
        std::fprintf(stderr, "could not write into %s\n", argv[2]);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return made(argc, argv);
    } catch (...) {
        std::fprintf(stderr, "the seeds could not be made\n");
        return 1;
    }
}
