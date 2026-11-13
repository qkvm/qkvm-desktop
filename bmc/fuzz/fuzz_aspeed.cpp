#include "fuzz_input.h"

#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// libFuzzer's way in (outside the ordinary build: bmc/CMakeLists.txt, QKVM_FUZZ). Up to four frames into one
// screen, each held to the reader's promises; the address sanitizer looks at every access, and libFuzzer's own
// limits (-malloc_limit_mb, -timeout) at what is asked for and how long it takes.
using namespace qkvm::bmc;

namespace {

[[noreturn]] void broken(const char* promise)
{
    std::fprintf(stderr, "a promise broken: %s\n", promise);
    std::abort();
}

bool sayable(const aspeed::FrameHeader& header)
{
    return 0 < header.width && header.width <= aspeed::largest && 0 < header.height &&
           header.height <= aspeed::largest && 0 <= header.strength && header.strength < 8 &&
           0 <= header.correctionStrength && header.correctionStrength < 8;
}

} // namespace

// (libFuzzer's name for it.)
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    aspeed::Screen screen;
    std::span<const std::uint8_t> rest(data, size);
    for (int frame = 0; frame < fuzz::mostFrames && rest.size() >= fuzz::headerBytes; ++frame) {
        const aspeed::FrameHeader header = fuzz::headerOf(rest);
        const std::size_t length =
            std::min<std::size_t>(fuzz::lengthOf(rest.subspan(12, 4)), rest.size() - fuzz::headerBytes);
        const std::span<const std::uint8_t> body = rest.subspan(fuzz::headerBytes, length);
        rest = rest.subspan(fuzz::headerBytes + length);
        const auto read = screen.read(header, body);
        if (read.ok()) {
            const int side = header.halfColour ? 16 : 8;
            const std::int64_t allowed =
                2 * std::int64_t{(header.width + side - 1) / side} * ((header.height + side - 1) / side) +
                aspeed::spare;
            if (!sayable(header)) {
                broken("a frame of an impossible header was read");
            }
            if (read.value() < 0 || read.value() > allowed) {
                broken("a frame changed more blocks than it may bring");
            }
        } else if (read.error().code != "garbled") {
            broken("a frame was refused for no reason there is");
        }
        const aspeed::Picture& picture = screen.picture();
        if (picture.width < 0 || picture.width > aspeed::largest || picture.height < 0 ||
            picture.height > aspeed::largest ||
            picture.rgb.size() !=
                3 * static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height)) {
            broken("the picture is not a whole picture");
        }
    }
    return 0;
}
