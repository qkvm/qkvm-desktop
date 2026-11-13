#include <qkvm/bmc/ami_console.h>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

// libFuzzer's way into the console's protocol (outside the ordinary build: bmc/CMakeLists.txt, QKVM_FUZZ). The
// input is what a controller sends, cut into feeds of the size its first two bytes say; every video message goes
// to the frames, and every whole frame to a screen, as the program will take them. Each step is held to its
// promises; the address sanitizer looks at every access, and libFuzzer's limits at room and time.
using namespace qkvm::bmc;

namespace {

[[noreturn]] void broken(const char* promise)
{
    std::fprintf(stderr, "a promise broken: %s\n", promise);
    std::abort();
}

} // namespace

// (libFuzzer's name for it.)
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2) {
        return 0;
    }
    const std::size_t step = std::max<std::size_t>(1, std::size_t{data[0]} | (std::size_t{data[1]} << 8));
    const std::span<const std::uint8_t> stream(data + 2, size - 2);
    ami::Messages arriving;
    ami::Frames frames;
    aspeed::Screen screen;
    for (std::size_t at = 0; at < stream.size(); at += step) {
        const auto found = arriving.feed(stream.subspan(at, std::min(step, stream.size() - at)));
        if (!found.ok()) {
            return 0; // (the program reads no further)
        }
        if (found.value().size() > ami::mostAtOnce) {
            broken("more messages at once than a console sends");
        }
        if (arriving.held() >= ami::headBytes + ami::longest) {
            broken("more kept than one message not yet whole");
        }
        for (const ami::Message& message : found.value()) {
            if (message.body.size() > ami::longest) {
                broken("a message longer than any");
            }
            if (message.kind != ami::kind::video) {
                continue;
            }
            const auto whole = frames.piece(message.body);
            if (!whole.ok()) {
                frames = ami::Frames(); // (as the web's begins again)
                continue;
            }
            if (frames.held() > ami::longest) {
                broken("more kept than a frame may be");
            }
            const std::optional<ami::WholeFrame>& made = whole.value();
            if (made) {
                if (made->data.size() > ami::longest) {
                    broken("a frame longer than any");
                }
                (void)screen.read(made->header, made->data);
            }
        }
    }
    return 0;
}
