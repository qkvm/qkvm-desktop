#pragma once

#include <qkvm/base/fuzz_promise.h>
#include <qkvm/bmc/ami_console.h>
#include <qkvm/bmc/aspeed.h>

#include <algorithm>
#include <cstdint>
#include <span>

// The harness of AMI's console protocol (fuzz_ami_console.cpp, libFuzzer's way in; test_core_fuzz_found, what it once
// found). The input is what a controller sends, cut into feeds of the size its first two bytes say; every video message
// goes to the frames, and every whole frame to a screen, as the program takes them. Each step is held to its promises;
// the address sanitizer looks at every access, and libFuzzer's limits at room and time.
//
// Its canary (fuzz_promise.h): two whole frames gathered from the console's video messages, fed in parts of fewer than
// eight bytes (so each message, and each frame's facts, made whole again across part after part), each read into the
// screen and changing blocks of it (every seed's parts are larger).
namespace qkvm::fuzz {

inline void readAmiConsole(std::span<const std::uint8_t> input)
{
    namespace ami = bmc::ami;
    if (input.size() < 2) {
        return;
    }
    const std::size_t step = std::max<std::size_t>(1, std::size_t{input[0]} | (std::size_t{input[1]} << 8));
    const std::span<const std::uint8_t> stream = input.subspan(2);
    ami::Messages arriving;
    ami::Frames frames;
    bmc::aspeed::Screen screen;
    int drawn = 0;
    for (std::size_t at = 0; at < stream.size(); at += step) {
        const auto found = arriving.feed(stream.subspan(at, std::min(step, stream.size() - at)));
        if (!found.ok()) {
            return; // (the program reads no further)
        }
        promise(found.value().size() <= ami::mostAtOnce, "more messages at once than a console sends");
        promise(arriving.held() < ami::headBytes + ami::longest, "more kept than one message not yet whole");
        for (const ami::Message& message : found.value()) {
            promise(message.body.size() <= ami::longest, "a message longer than any");
            if (message.kind != ami::kind::video) {
                continue;
            }
            const auto whole = frames.piece(message.body);
            if (!whole.ok()) {
                frames = ami::Frames(); // (as the web's begins again)
                continue;
            }
            promise(frames.held() <= ami::longest, "more kept than a frame may be");
            const std::optional<ami::WholeFrame>& made = whole.value();
            if (made) {
                promise(made->data.size() <= ami::longest, "a frame longer than any");
                const base::Result<int> read = screen.read(made->header, made->data);
                if (read.ok() && read.value() > 0 && ++drawn == 2 && step < 8) {
                    reached("two whole frames of the console, fed in parts of under eight bytes, each drawn");
                }
            }
        }
    }
}

} // namespace qkvm::fuzz
